/*******************************************************************************
 * File Name:   buffer_mgr.c
 *
 * Description: Two-tier store-and-forward buffer implementation.
 *
 *              Tier 1: RAM ring buffer (fast, volatile)
 *              Tier 2: Flash-backed persistent buffer (Em_EEPROM, survives reboot)
 *
 *              The RAM ring buffer stores pre-encoded JSON + topic strings.
 *              Size is determined by g_config.buffer.ram_max_records.
 *
 *              When flash_max_records > 0, records that cannot fit in RAM
 *              are spilled to flash.  On boot, any records recovered from
 *              flash are flushed before new RAM records.
 *
 *              Thread safety: taskENTER_CRITICAL / taskEXIT_CRITICAL
 *              (short critical sections — only pointer manipulation).
 *              Flash writes happen outside critical sections (they block ~16ms).
 *
 * Related Document: agent.md §6 (Task C), §8, docs/persistent_buffer.md
 *
 ******************************************************************************/

#include "buffer_mgr.h"
#include "persistent_buffer.h"
#include "gateway_config.h"
#include "gateway_ipc.h"
#include "metrics.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>
#include <string.h>

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

/*******************************************************************************
 * Initialization
 ******************************************************************************/
bool buffer_mgr_init(void)
{
    if (!g_config.buffer.enabled)
    {
        printf("[BUF] Buffer disabled in config\n");
        return true;  /* Not an error — just nothing to do */
    }

    s_capacity = g_config.buffer.ram_max_records;
    if (s_capacity == 0u)
    {
        printf("[BUF] WARN: ram_max_records=0, buffer effectively disabled\n");
        return true;
    }

    s_ring = (buffer_record_t *)pvPortMalloc(
        (size_t)s_capacity * sizeof(buffer_record_t));

    if (s_ring == NULL)
    {
        printf("[BUF] ERROR: Failed to allocate %u × %u bytes for ring buffer\n",
               (unsigned)s_capacity, (unsigned)sizeof(buffer_record_t));
        return false;
    }

    s_head  = 0u;
    s_tail  = 0u;
    s_count = 0u;

    printf("[BUF] Ring buffer initialised: %u records × %u bytes = %u bytes\n",
           (unsigned)s_capacity,
           (unsigned)sizeof(buffer_record_t),
           (unsigned)((uint32_t)s_capacity * sizeof(buffer_record_t)));

    /* Initialise flash tier (if configured) */
    if (g_config.buffer.flash_max_records > 0u)
    {
        if (!persistent_buffer_init())
        {
            printf("[BUF] WARNING: Persistent buffer init failed, flash tier disabled\n");
            /* Continue with RAM-only — not a fatal error */
        }
        else
        {
            printf("[BUF] Persistent backend: %s\n", PERSISTENT_BACKEND_NAME);
        }
    }

    return true;
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

        /* RAM full — try to spill to flash if enabled.
         * NOTE: flash_buffer_put() blocks ~16ms but is called outside
         * the critical section.  This is acceptable because buffer_mgr_put
         * is called from mqtt_gw_task (medium priority) and the flash write
         * does not hold any mutex needed by other tasks. */
        if (g_config.buffer.flash_max_records > 0u)
        {
            bool spilled = persistent_buffer_put(topic, payload, payload_len);
            if (spilled)
            {
                return true;  /* Metrics already updated by flash_buffer_put */
            }
            /* Flash also full — fall through to drop policy */
        }

        taskENTER_CRITICAL();

        /* Re-check (another task may have consumed) */
        if (s_count >= s_capacity)
        {
            if (g_config.buffer.drop_oldest)
            {
                /* Overwrite oldest: advance tail */
                s_tail = (s_tail + 1u) % s_capacity;
                s_count--;
                metrics_inc_buffer_dropped();
                taskEXIT_CRITICAL();
                gateway_ipc_post_event(EVT_BUFFER_OVERFLOW, "drop_oldest");
                taskENTER_CRITICAL();
            }
            else
            {
                /* Drop new record */
                taskEXIT_CRITICAL();
                metrics_inc_buffer_dropped();
                gateway_ipc_post_event(EVT_BUFFER_OVERFLOW, "drop_newest");
                return false;
            }
        }
    }

    /* Write at head */
    buffer_record_t *rec = &s_ring[s_head];
    strncpy(rec->topic, topic, BUFFER_TOPIC_MAX - 1u);
    rec->topic[BUFFER_TOPIC_MAX - 1u] = '\0';

    uint16_t copy_len = payload_len;
    if (copy_len > BUFFER_PAYLOAD_MAX - 1u) copy_len = BUFFER_PAYLOAD_MAX - 1u;
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
 * Peek (read without removing — preserves FIFO ordering)
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

/*******************************************************************************
 * Consume (remove the record previously returned by peek)
 ******************************************************************************/
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
 * Buffer flush task (Task C)
 *
 * This task no longer publishes to MQTT directly (P0 fix: all cy_mqtt_publish
 * calls are serialised through mqtt_gw_task).  Instead, it:
 *   - Updates buffer depth gauges periodically
 *   - Could be extended for flash spill management in the future
 *
 * The actual flush (peek → publish → consume) is done inside mqtt_gw_task
 * after draining the telemetry/status/event queues.
 ******************************************************************************/

void buffer_task(void *pvParameters)
{
    (void)pvParameters;

    printf("[BUF] Buffer task started\n");

    if (!g_config.buffer.enabled || s_capacity == 0u)
    {
        printf("[BUF] Buffering disabled, task idle\n");
        for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
    }

    const bool flash_enabled = (g_config.buffer.flash_max_records > 0u);

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(g_config.buffer.flush_interval_ms));

        /* Update gauges for metrics reporting */
        metrics_set_buffer_depth_ram(buffer_mgr_depth());
        if (flash_enabled)
        {
            metrics_set_buffer_depth_flash(persistent_buffer_depth());
        }
    }
}

/* [] END OF FILE */
