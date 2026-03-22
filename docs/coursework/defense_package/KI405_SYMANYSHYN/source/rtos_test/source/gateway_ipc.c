/*******************************************************************************
 * File Name:   gateway_ipc.c
 *
 * Description: Implementation of inter-task communication primitives.
 *
 * Related Document: agent.md §6
 *
 ******************************************************************************/

#include "gateway_ipc.h"
#include "wallclock.h"
#include "metrics.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

/*******************************************************************************
 * Private data
 ******************************************************************************/
static QueueHandle_t s_telemetry_q;
static QueueHandle_t s_status_q;
static QueueHandle_t s_event_q;

/** MQTT online flag — volatile for cross-task reads (single writer pattern) */
static volatile bool s_mqtt_online;

/** Global monotonic sequence counter */
static volatile uint32_t s_seq_counter;

/*******************************************************************************
 * Initialization
 ******************************************************************************/
bool gateway_ipc_init(void)
{
    s_telemetry_q = xQueueCreate(IPC_TELEMETRY_QUEUE_DEPTH,
                                 sizeof(telemetry_record_t));
    s_status_q    = xQueueCreate(IPC_STATUS_QUEUE_DEPTH,
                                 sizeof(status_record_t));
    s_event_q     = xQueueCreate(IPC_EVENT_QUEUE_DEPTH,
                                 sizeof(event_record_t));

    if (s_telemetry_q == NULL || s_status_q == NULL || s_event_q == NULL)
    {
        printf("[IPC] ERROR: Failed to create queues\n");
        return false;
    }

    s_mqtt_online  = false;
    s_seq_counter  = 0u;

    printf("[IPC] Queues created: telem=%u status=%u event=%u\n",
           IPC_TELEMETRY_QUEUE_DEPTH, IPC_STATUS_QUEUE_DEPTH,
           IPC_EVENT_QUEUE_DEPTH);

    return true;
}

/*******************************************************************************
 * Queue accessors
 ******************************************************************************/
QueueHandle_t gateway_ipc_telemetry_queue(void) { return s_telemetry_q; }
QueueHandle_t gateway_ipc_status_queue(void)    { return s_status_q;    }
QueueHandle_t gateway_ipc_event_queue(void)     { return s_event_q;     }
uint32_t gateway_ipc_telemetry_queue_depth(void)
{
    return (s_telemetry_q != NULL) ?
        (uint32_t)uxQueueMessagesWaiting(s_telemetry_q) : 0u;
}

/*******************************************************************************
 * MQTT state
 ******************************************************************************/
void gateway_ipc_set_mqtt_online(bool online)
{
    s_mqtt_online = online;
}

bool gateway_ipc_is_mqtt_online(void)
{
    return s_mqtt_online;
}

/*******************************************************************************
 * Sequence counter
 ******************************************************************************/
uint32_t gateway_ipc_next_seq(void)
{
    /*
     * Cortex-M4: 32-bit aligned reads/writes are atomic.
     * Only pmbus_poll_task calls this, so a simple increment is safe.
     * If multiple tasks ever need it, switch to taskENTER_CRITICAL().
     */
    taskENTER_CRITICAL();
    uint32_t seq = s_seq_counter++;
    taskEXIT_CRITICAL();
    return seq;
}

/*******************************************************************************
 * Monotonic time
 ******************************************************************************/
uint32_t gateway_ipc_monotonic_ms(void)
{
    return (uint32_t)xTaskGetTickCount() * (uint32_t)portTICK_PERIOD_MS;
}

/*******************************************************************************
 * Timestamp
 ******************************************************************************/
uint64_t gateway_ipc_now_ms(void)
{
    return wallclock_now_ms();
}

/*******************************************************************************
 * Event posting convenience
 ******************************************************************************/
void gateway_ipc_post_event(event_type_t type, const char *detail)
{
    event_record_t evt;
    memset(&evt, 0, sizeof(evt));

    evt.ts_ms = gateway_ipc_now_ms();
    evt.time_synced = wallclock_is_synced();
    evt.type  = type;

    if (detail != NULL)
    {
        strncpy(evt.detail, detail, EVT_DETAIL_MAX - 1u);
        evt.detail[EVT_DETAIL_MAX - 1u] = '\0';
    }

    if (xQueueSend(s_event_q, &evt, 0) != pdTRUE)
    {
        printf("[IPC] WARN: event queue full, dropped %s\n",
               event_type_str(type));
        metrics_inc_queue_drops();
    }
}

/* [] END OF FILE */
