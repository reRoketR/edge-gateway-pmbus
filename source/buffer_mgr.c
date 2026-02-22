/*******************************************************************************
 * File Name:   buffer_mgr.c
 *
 * Description: RAM-only store-and-forward ring buffer implementation.
 *
 *              The ring buffer stores pre-encoded JSON + topic strings.
 *              Size is determined by g_config.buffer.ram_max_records.
 *
 *              Thread safety: taskENTER_CRITICAL / taskEXIT_CRITICAL
 *              (short critical sections — only pointer manipulation).
 *
 * Related Document: agent.md §6 (Task C), §8
 *
 ******************************************************************************/

#include "buffer_mgr.h"
#include "gateway_config.h"
#include "gateway_ipc.h"
#include "metrics.h"

#include "FreeRTOS.h"
#include "task.h"

/* For MQTT publish during flush */
#include "cy_mqtt_api.h"
#include "mqtt_client_config.h"

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
        if (g_config.buffer.drop_oldest)
        {
            /* Overwrite oldest: advance tail */
            s_tail = (s_tail + 1u) % s_capacity;
            s_count--;
            metrics_inc_buffer_dropped();
        }
        else
        {
            /* Drop new record */
            taskEXIT_CRITICAL();
            metrics_inc_buffer_dropped();
            return false;
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
 * Get
 ******************************************************************************/
bool buffer_mgr_get(buffer_record_t *out)
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
 ******************************************************************************/

/* External MQTT connection handle (defined in mqtt_client_config.c or mqtt_task) */
extern cy_mqtt_t mqtt_connection;

void buffer_task(void *pvParameters)
{
    (void)pvParameters;

    printf("[BUF] Buffer task started\n");

    if (!g_config.buffer.enabled || s_capacity == 0u)
    {
        printf("[BUF] Buffering disabled, task idle\n");
        for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
    }

    buffer_record_t rec;

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(g_config.buffer.flush_interval_ms));

        /* Only flush when MQTT is online */
        if (!gateway_ipc_is_mqtt_online())
        {
            /* Update gauge */
            metrics_set_buffer_depth_ram(buffer_mgr_depth());
            continue;
        }

        /* Flush up to flush_batch_size records */
        uint16_t flushed = 0u;
        while (flushed < g_config.buffer.flush_batch_size &&
               buffer_mgr_get(&rec))
        {
            cy_mqtt_publish_info_t pub = {
                .qos         = (cy_mqtt_qos_t)g_config.mqtt.qos,
                .retain      = false,
                .dup         = false,
                .topic       = rec.topic,
                .topic_len   = (uint16_t)strlen(rec.topic),
                .payload     = rec.payload,
                .payload_len = rec.payload_len,
            };

            cy_rslt_t res = cy_mqtt_publish(mqtt_connection, &pub);
            if (res != CY_RSLT_SUCCESS)
            {
                /* Put it back (best-effort; may fail if full) */
                buffer_mgr_put(rec.topic, rec.payload, rec.payload_len);
                metrics_inc_mqtt_pub_fail();
                break;  /* Stop flushing on first failure */
            }
            else
            {
                metrics_inc_mqtt_pub_ok();
                flushed++;
            }
        }

        /* Update gauge */
        metrics_set_buffer_depth_ram(buffer_mgr_depth());

        if (flushed > 0u)
        {
            printf("[BUF] Flushed %u buffered records\n", (unsigned)flushed);
        }
    }
}

/* [] END OF FILE */
