/*******************************************************************************
 * File Name:   buffer_mgr.c
 *
 * Description: Two-tier store-and-forward buffer implementation.
 *
 *              Tier 1: RAM ring buffer (fast, volatile)
 *              Tier 2: Optional persistent buffer backend
 *
 *              The RAM ring buffer stores pre-encoded JSON + topic strings.
 *              Size is determined by g_config.buffer.ram_max_records.
 *
 *              When flash_max_records > 0, records that cannot fit in RAM
 *              are spilled to the persistent tier. On boot, any records
 *              recovered from the persistent tier are flushed before new RAM
 *              records.
 *
 *              Thread safety: taskENTER_CRITICAL / taskEXIT_CRITICAL
 *              (short critical sections, only pointer manipulation).
 *
 ******************************************************************************/

#include "buffer_mgr.h"
#include "persistent_buffer.h"
#include "gateway_config.h"
#include "gateway_ipc.h"
#include "metrics.h"
#include "telemetry.h"
#include "events.h"
#include "emergency_ring.h"
#if defined(BUFFER_BACKEND_QSPI)
#include "qspi_flash.h"
#endif

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#ifdef BUFFER_MGR_HOST_TEST
#define BMC_STATIC  /* empty — expose drain_once() for host tests */
#else
#define BMC_STATIC static
#endif

#include <stdio.h>
#include <string.h>

/*******************************************************************************
 * Spill task constants and scratch buffers
 ******************************************************************************/

/** Max idle wait when no producer notification arrives (ms) */
#define SPILL_IDLE_WAIT_MAX_MS   1000u

/** JSON encoding buffer (max observed telemetry ~232 bytes) */
#define SPILL_JSON_BUF_SIZE   512u

/** Topic string buffer */
#define SPILL_TOPIC_BUF_SIZE  80u

/** Scratch buffers — owned exclusively by buffer_task, no concurrency risk */
static char s_spill_json[SPILL_JSON_BUF_SIZE];
static char s_spill_topic[SPILL_TOPIC_BUF_SIZE];

/*******************************************************************************
 * Private data
 ******************************************************************************/

/** Ring buffer (dynamically allocated from FreeRTOS heap at init) */
static buffer_record_t *s_ring = NULL;

/** Ring buffer capacity (from config) */
static uint16_t s_capacity = 0u;

/** Head = next write position, tail = next read position */
static uint16_t s_head = 0u;
static uint16_t s_tail = 0u;

/** Current count of records in the buffer */
static uint16_t s_count = 0u;

/** Persistent tier configuration/state */
static bool s_persistent_requested = false;
static bool s_persistent_ready = false;
static bool s_persistent_init_attempted = false;
static uint32_t s_current_boot_gen = 1u;
static TaskHandle_t s_flush_task_handle = NULL;
static TaskHandle_t s_spill_task_handle = NULL;

static bool buffer_mgr_put_internal(const char *topic,
                                    const char *payload,
                                    uint16_t payload_len,
                                    uint32_t origin_read_start_ms,
                                    uint32_t origin_boot_gen);
static void buffer_mgr_notify_flush_task(void);
BMC_STATIC void buffer_mgr_drain_once(void);

static void buffer_mgr_try_init_persistent(void)
{
    if (!s_persistent_requested || s_persistent_ready || s_persistent_init_attempted)
    {
        return;
    }

#if defined(BUFFER_BACKEND_QSPI)
    if (qspi_flash_get_size() == 0u)
    {
        printf("[BUF] WARNING: QSPI backend selected but QSPI flash is not ready, persistent tier disabled\n");
        s_persistent_requested = false;
        return;
    }
#endif

    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
    {
        return;
    }

    s_persistent_init_attempted = true;

    if (!persistent_buffer_init())
    {
        printf("[BUF] WARNING: Persistent buffer init failed, flash tier disabled\n");
        return;
    }

    s_persistent_ready = true;
    s_current_boot_gen = persistent_buffer_total_writes() + 1u;
    if (s_current_boot_gen == 0u)
    {
        s_current_boot_gen = 1u;
    }
    printf("[BUF] Persistent backend: %s\n", PERSISTENT_BACKEND_NAME);
}

/*******************************************************************************
 * Initialization
 ******************************************************************************/
bool buffer_mgr_init(void)
{
    if (!g_config.buffer.enabled)
    {
        printf("[BUF] Buffer disabled in config\n");
        return true;
    }

    s_capacity = g_config.buffer.ram_max_records;
    if (s_capacity == 0u)
    {
        printf("[BUF] WARN: ram_max_records=0, buffer effectively disabled\n");
        return true;
    }

    s_ring = (buffer_record_t *)pvPortMalloc((size_t)s_capacity * sizeof(buffer_record_t));
    if (s_ring == NULL)
    {
        printf("[BUF] ERROR: Failed to allocate %u x %u bytes for ring buffer\n",
               (unsigned)s_capacity,
               (unsigned)sizeof(buffer_record_t));
        return false;
    }

    s_head = 0u;
    s_tail = 0u;
    s_count = 0u;

    printf("[BUF] Ring buffer initialised: %u records x %u bytes = %u bytes\n",
           (unsigned)s_capacity,
           (unsigned)sizeof(buffer_record_t),
           (unsigned)((uint32_t)s_capacity * sizeof(buffer_record_t)));

    s_persistent_requested = (g_config.buffer.flash_max_records > 0u);
    s_persistent_ready = false;
    s_persistent_init_attempted = false;
    s_current_boot_gen = 1u;

    /* Defer persistent tier init until the scheduler is running because
     * some backends (QSPI serial-flash) use RTOS mutexes for erase/write.
     */
    if (s_persistent_requested &&
        xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
    {
        printf("[BUF] Persistent backend deferred until scheduler start\n");
    }
    else
    {
        buffer_mgr_try_init_persistent();
    }

    return true;
}

void buffer_mgr_late_init(void)
{
    buffer_mgr_try_init_persistent();
}

void buffer_mgr_register_flush_task(TaskHandle_t handle)
{
    taskENTER_CRITICAL();
    s_flush_task_handle = handle;
    taskEXIT_CRITICAL();
}

void buffer_mgr_register_spill_task(TaskHandle_t handle)
{
    taskENTER_CRITICAL();
    s_spill_task_handle = handle;
    taskEXIT_CRITICAL();
}

void buffer_mgr_signal_spill_task(void)
{
    TaskHandle_t spill_task_handle;

    taskENTER_CRITICAL();
    spill_task_handle = s_spill_task_handle;
    taskEXIT_CRITICAL();

    if (spill_task_handle != NULL)
    {
        (void)xTaskNotifyGive(spill_task_handle);
    }
}

/*******************************************************************************
 * Put
 ******************************************************************************/
static bool buffer_mgr_put_internal(const char *topic,
                                    const char *payload,
                                    uint16_t payload_len,
                                    uint32_t origin_read_start_ms,
                                    uint32_t origin_boot_gen)
{
    if (s_ring == NULL || s_capacity == 0u ||
        topic == NULL || payload == NULL || payload_len == 0u)
    {
        return false;
    }

    buffer_record_t incoming;
    memset(&incoming, 0, sizeof(incoming));

    strncpy(incoming.topic, topic, BUFFER_TOPIC_MAX - 1u);
    incoming.topic[BUFFER_TOPIC_MAX - 1u] = '\0';

    uint16_t copy_len = payload_len;
    if (copy_len > BUFFER_PAYLOAD_MAX - 1u)
    {
        copy_len = BUFFER_PAYLOAD_MAX - 1u;
    }
    memcpy(incoming.payload, payload, copy_len);
    incoming.payload[copy_len] = '\0';
    incoming.payload_len = copy_len;
    incoming.origin_read_start_ms = origin_read_start_ms;
    incoming.origin_boot_gen = origin_boot_gen;

    taskENTER_CRITICAL();

    if (s_count >= s_capacity)
    {
        taskEXIT_CRITICAL();

        if (s_persistent_ready)
        {
            bool spilled = persistent_buffer_put_record(&incoming);
            if (spilled)
            {
                return true; /* Metrics are updated by the persistent backend */
            }
        }

        taskENTER_CRITICAL();

        /* Re-check after leaving the critical section */
        if (s_count >= s_capacity)
        {
            if (g_config.buffer.drop_oldest)
            {
                s_tail = (s_tail + 1u) % s_capacity;
                s_count--;
                metrics_inc_buffer_dropped();
                taskEXIT_CRITICAL();
                gateway_ipc_post_event(EVT_BUFFER_OVERFLOW, "drop_oldest");
                taskENTER_CRITICAL();
            }
            else
            {
                taskEXIT_CRITICAL();
                metrics_inc_buffer_dropped();
                gateway_ipc_post_event(EVT_BUFFER_OVERFLOW, "drop_newest");
                return false;
            }
        }
    }

    s_ring[s_head] = incoming;

    s_head = (s_head + 1u) % s_capacity;
    s_count++;

    taskEXIT_CRITICAL();

    metrics_inc_buffer_enqueued();
    return true;
}

bool buffer_mgr_put(const char *topic, const char *payload, uint16_t payload_len)
{
    return buffer_mgr_put_internal(topic, payload, payload_len, 0u, 0u);
}

uint32_t buffer_mgr_current_boot_gen(void)
{
    return s_current_boot_gen;
}

/*******************************************************************************
 * Peek / Consume
 ******************************************************************************/
bool buffer_mgr_peek(buffer_record_t *out)
{
    if (s_ring == NULL || out == NULL)
    {
        return false;
    }

    taskENTER_CRITICAL();

    if (s_count == 0u)
    {
        taskEXIT_CRITICAL();
        return false;
    }

    memcpy(out, &s_ring[s_tail], sizeof(buffer_record_t));

    taskEXIT_CRITICAL();
    return true;
}

bool buffer_mgr_consume(void)
{
    if (s_ring == NULL)
    {
        return false;
    }

    taskENTER_CRITICAL();

    if (s_count == 0u)
    {
        taskEXIT_CRITICAL();
        return false;
    }

    s_tail = (s_tail + 1u) % s_capacity;
    s_count--;

    taskEXIT_CRITICAL();

    metrics_inc_buffer_dequeued();
    return true;
}

/*******************************************************************************
 * Depth
 ******************************************************************************/
uint32_t buffer_mgr_depth(void)
{
    taskENTER_CRITICAL();
    uint32_t depth = s_count;
    taskEXIT_CRITICAL();
    return depth;
}

static void buffer_mgr_notify_flush_task(void)
{
    TaskHandle_t flush_task_handle;

    taskENTER_CRITICAL();
    flush_task_handle = s_flush_task_handle;
    taskEXIT_CRITICAL();

    if (flush_task_handle != NULL)
    {
        (void)xTaskNotifyGive(flush_task_handle);
    }
}

/*******************************************************************************
 * Spill task — drain helpers
 *
 * These functions evacuate upstream FreeRTOS queues and the emergency ring
 * into buffer_mgr.  They run exclusively inside buffer_task (single writer).
 ******************************************************************************/

static bool drain_emergency_ring(void)
{
    bool did_enqueue = false;
    telemetry_record_t rec;
    while (emergency_ring_get(&rec))
    {
        int len = encode_telemetry_json(&rec, s_spill_json, SPILL_JSON_BUF_SIZE);
        if (len <= 0) continue;
        int tl = build_device_topic(s_spill_topic, SPILL_TOPIC_BUF_SIZE,
                                     rec.addr_7bit, "telemetry");
        if (tl <= 0) continue;
        if (buffer_mgr_put_internal(s_spill_topic, s_spill_json, (uint16_t)len,
                                    rec.read_start_ms, s_current_boot_gen))
        {
            did_enqueue = true;
        }
    }

    return did_enqueue;
}

static bool drain_telemetry_queue(void)
{
    bool did_enqueue = false;
    telemetry_record_t rec;
    while (xQueueReceive(gateway_ipc_telemetry_queue(), &rec, 0) == pdTRUE)
    {
        int len = encode_telemetry_json(&rec, s_spill_json, SPILL_JSON_BUF_SIZE);
        if (len <= 0) continue;
        int tl = build_device_topic(s_spill_topic, SPILL_TOPIC_BUF_SIZE,
                                     rec.addr_7bit, "telemetry");
        if (tl <= 0) continue;
        if (buffer_mgr_put_internal(s_spill_topic, s_spill_json, (uint16_t)len,
                                    rec.read_start_ms, s_current_boot_gen))
        {
            did_enqueue = true;
        }
    }

    return did_enqueue;
}

static bool drain_status_queue(void)
{
    bool did_enqueue = false;
    status_record_t rec;
    while (xQueueReceive(gateway_ipc_status_queue(), &rec, 0) == pdTRUE)
    {
        int len = encode_status_json(&rec, s_spill_json, SPILL_JSON_BUF_SIZE);
        if (len <= 0) continue;
        int tl = build_device_topic(s_spill_topic, SPILL_TOPIC_BUF_SIZE,
                                     rec.addr_7bit, "status");
        if (tl <= 0) continue;
        if (buffer_mgr_put(s_spill_topic, s_spill_json, (uint16_t)len))
        {
            did_enqueue = true;
        }
    }

    return did_enqueue;
}

static bool drain_event_queue(void)
{
    bool did_enqueue = false;
    event_record_t evt;
    while (xQueueReceive(gateway_ipc_event_queue(), &evt, 0) == pdTRUE)
    {
        int len = encode_event_json(&evt, s_spill_json, SPILL_JSON_BUF_SIZE);
        if (len <= 0) continue;
        int tl = build_events_topic(s_spill_topic, SPILL_TOPIC_BUF_SIZE);
        if (tl <= 0) continue;
        if (buffer_mgr_put(s_spill_topic, s_spill_json, (uint16_t)len))
        {
            did_enqueue = true;
        }
    }

    return did_enqueue;
}

BMC_STATIC void buffer_mgr_drain_once(void)
{
    bool did_enqueue = false;

    did_enqueue |= drain_emergency_ring();
    did_enqueue |= drain_telemetry_queue();
    did_enqueue |= drain_status_queue();
    did_enqueue |= drain_event_queue();

    if (did_enqueue)
    {
        buffer_mgr_notify_flush_task();
    }

    metrics_set_buffer_depth_ram(buffer_mgr_depth());
    if (s_persistent_ready)
    {
        metrics_set_buffer_depth_flash(persistent_buffer_depth());
    }
    else
    {
        metrics_set_buffer_depth_flash(0u);
    }
}

/*******************************************************************************
 * Spill task (Task C) — continuous queue evacuation
 *
 * Decouples queue draining from the MQTT task.  This task is the ONLY writer
 * to buffer_mgr, while the MQTT task is the ONLY reader (peek/consume/flush).
 ******************************************************************************/
void buffer_task(void *pvParameters)
{
    (void)pvParameters;

    printf("[BUF] Spill task started\n");
    buffer_mgr_register_spill_task(xTaskGetCurrentTaskHandle());

    if (!g_config.buffer.enabled || s_capacity == 0u)
    {
        printf("[BUF] Buffering disabled, spill task idle\n");
        for (;;)
        {
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }

    buffer_mgr_drain_once();

    for (;;)
    {

        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SPILL_IDLE_WAIT_MAX_MS));
        buffer_mgr_drain_once();



        /* Sleep — queues accumulate while we sleep, drained on next wake */
    }
}

/* [] END OF FILE */
