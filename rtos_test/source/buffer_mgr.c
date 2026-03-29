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

#include <stdio.h>
#include <string.h>

/*******************************************************************************
 * Spill task constants and scratch buffers
 ******************************************************************************/

/** How often the spill task polls upstream queues (ms) */
#define SPILL_POLL_MS   50u

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

/*******************************************************************************
 * Put
 ******************************************************************************/
bool buffer_mgr_put(const char *topic, const char *payload, uint16_t payload_len)
{
    if (s_ring == NULL || s_capacity == 0u)
    {
        return false;
    }

    taskENTER_CRITICAL();

    if (s_count >= s_capacity)
    {
        taskEXIT_CRITICAL();

        if (s_persistent_ready)
        {
            bool spilled = persistent_buffer_put(topic, payload, payload_len);
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

    buffer_record_t *rec = &s_ring[s_head];
    strncpy(rec->topic, topic, BUFFER_TOPIC_MAX - 1u);
    rec->topic[BUFFER_TOPIC_MAX - 1u] = '\0';

    uint16_t copy_len = payload_len;
    if (copy_len > BUFFER_PAYLOAD_MAX - 1u)
    {
        copy_len = BUFFER_PAYLOAD_MAX - 1u;
    }
    memcpy(rec->payload, payload, copy_len);
    rec->payload[copy_len] = '\0';
    rec->payload_len = copy_len;

    s_head = (s_head + 1u) % s_capacity;
    s_count++;

    taskEXIT_CRITICAL();

    metrics_inc_buffer_enqueued();
    return true;
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

/*******************************************************************************
 * Spill task — drain helpers
 *
 * These functions evacuate upstream FreeRTOS queues and the emergency ring
 * into buffer_mgr.  They run exclusively inside buffer_task (single writer).
 ******************************************************************************/

static void drain_emergency_ring(void)
{
    telemetry_record_t rec;
    while (emergency_ring_get(&rec))
    {
        int len = encode_telemetry_json(&rec, s_spill_json, SPILL_JSON_BUF_SIZE);
        if (len <= 0) continue;
        int tl = build_device_topic(s_spill_topic, SPILL_TOPIC_BUF_SIZE,
                                     rec.addr_7bit, "telemetry");
        if (tl <= 0) continue;
        buffer_mgr_put(s_spill_topic, s_spill_json, (uint16_t)len);
    }
}

static void drain_telemetry_queue(void)
{
    telemetry_record_t rec;
    while (xQueueReceive(gateway_ipc_telemetry_queue(), &rec, 0) == pdTRUE)
    {
        int len = encode_telemetry_json(&rec, s_spill_json, SPILL_JSON_BUF_SIZE);
        if (len <= 0) continue;
        int tl = build_device_topic(s_spill_topic, SPILL_TOPIC_BUF_SIZE,
                                     rec.addr_7bit, "telemetry");
        if (tl <= 0) continue;
        buffer_mgr_put(s_spill_topic, s_spill_json, (uint16_t)len);
    }
}

static void drain_status_queue(void)
{
    status_record_t rec;
    while (xQueueReceive(gateway_ipc_status_queue(), &rec, 0) == pdTRUE)
    {
        int len = encode_status_json(&rec, s_spill_json, SPILL_JSON_BUF_SIZE);
        if (len <= 0) continue;
        int tl = build_device_topic(s_spill_topic, SPILL_TOPIC_BUF_SIZE,
                                     rec.addr_7bit, "status");
        if (tl <= 0) continue;
        buffer_mgr_put(s_spill_topic, s_spill_json, (uint16_t)len);
    }
}

static void drain_event_queue(void)
{
    event_record_t evt;
    while (xQueueReceive(gateway_ipc_event_queue(), &evt, 0) == pdTRUE)
    {
        int len = encode_event_json(&evt, s_spill_json, SPILL_JSON_BUF_SIZE);
        if (len <= 0) continue;
        int tl = build_events_topic(s_spill_topic, SPILL_TOPIC_BUF_SIZE);
        if (tl <= 0) continue;
        buffer_mgr_put(s_spill_topic, s_spill_json, (uint16_t)len);
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

    if (!g_config.buffer.enabled || s_capacity == 0u)
    {
        printf("[BUF] Buffering disabled, spill task idle\n");
        for (;;)
        {
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }

    for (;;)
    {
        /* Drain all upstream queues into buffer_mgr */
        drain_emergency_ring();
        drain_telemetry_queue();
        drain_status_queue();
        drain_event_queue();

        /* Update gauge metrics */
        metrics_set_buffer_depth_ram(buffer_mgr_depth());
        if (s_persistent_ready)
        {
            metrics_set_buffer_depth_flash(persistent_buffer_depth());
        }
        else
        {
            metrics_set_buffer_depth_flash(0u);
        }

        /* Sleep — queues accumulate while we sleep, drained on next wake */
        vTaskDelay(pdMS_TO_TICKS(SPILL_POLL_MS));
    }
}

/* [] END OF FILE */
