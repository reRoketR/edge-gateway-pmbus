/**
 * @file gateway_ipc.h
 * @brief Inter-task communication primitives for the PMBus-MQTT gateway.
 * @ingroup gateway_ipc
 *
 * @details
 * Provides:
 *   - FreeRTOS queues for telemetry, status, and event records
 *   - MQTT online/offline state flag (atomic read from any task)
 *   - Global monotonic sequence counter
 *   - Wall-clock timestamp helper (Unix epoch ms after SNTP sync,
 *     ms-since-boot before sync - see wallclock.h)
 *
 * All queues use fixed-size items so no heap allocation is needed at runtime.
 * Queue depths are compile-time constants.
 *
 * @see agent.md section 6 (IPC section)
 *
 * @defgroup gateway_ipc Gateway IPC
 * @brief FreeRTOS queues, sequence counter, and shared state.
 * @{
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "FreeRTOS.h"
#include "queue.h"

#include "telemetry.h"
#include "events.h"
#include "cmd_handler.h"

/*******************************************************************************
 * Queue depths (compile-time, tune per available RAM)
 * Telemetry: 64 x ~78 bytes = ~5 KB - survives 128 s of 2 s-poll connect delay
 ******************************************************************************/
#define IPC_TELEMETRY_QUEUE_DEPTH   64u
#define IPC_STATUS_QUEUE_DEPTH      16u
#define IPC_EVENT_QUEUE_DEPTH       16u
#define IPC_CMD_RAW_QUEUE_DEPTH     CMD_QUEUE_DEPTH
#define IPC_CMD_REQUEST_QUEUE_DEPTH CMD_QUEUE_DEPTH
#define IPC_CMD_RESPONSE_QUEUE_DEPTH CMD_QUEUE_DEPTH

/*******************************************************************************
 * Initialization
 ******************************************************************************/

/**
 * @brief Create all IPC queues and initialise shared state.
 *
 * Must be called once before starting any gateway task.
 *
 * @return true on success, false if queue creation fails.
 */
bool gateway_ipc_init(void);

/*******************************************************************************
 * Queue handles (read-only after init)
 ******************************************************************************/

/** Queue of telemetry_record_t items (producer: pmbus_poll_task). */
QueueHandle_t gateway_ipc_telemetry_queue(void);

/** Queue of status_record_t items (producer: pmbus_poll_task). */
QueueHandle_t gateway_ipc_status_queue(void);

/** Queue of event_record_t items (producer: any task). */
QueueHandle_t gateway_ipc_event_queue(void);

/** Queue of cmd_raw_t items (producer: MQTT callback). */
QueueHandle_t gateway_ipc_cmd_raw_queue(void);

/** Queue of cmd_request_t items (producer: MQTT task). */
QueueHandle_t gateway_ipc_cmd_request_queue(void);

/** Queue of cmd_response_t items (producer: pmbus_poll_task). */
QueueHandle_t gateway_ipc_cmd_response_queue(void);

/** Current telemetry queue depth (0 if queue is not initialised yet). */
uint32_t gateway_ipc_telemetry_queue_depth(void);

/*******************************************************************************
 * MQTT state flag
 ******************************************************************************/

/** Set MQTT state (called by mqtt_task only). */
void gateway_ipc_set_mqtt_online(bool online);

/** Check MQTT state (safe to call from any task). */
bool gateway_ipc_is_mqtt_online(void);

/*******************************************************************************
 * Sequence counter (monotonic, shared across tasks)
 ******************************************************************************/

/**
 * @brief Get the next sequence number (atomically incremented).
 *
 * Uses a 32-bit counter that wraps around. One global counter is shared
 * by telemetry and status records per agent.md section 4.2. The counter is
 * checkpointed best-effort via persistent_seq, so reboot restores the last
 * checkpoint rather than resetting unconditionally to zero.
 *
 * @return Next sequence number.
 */
uint32_t gateway_ipc_next_seq(void);

/*******************************************************************************
 * Monotonic time helper
 ******************************************************************************/

/**
 * @brief Get monotonic uptime in milliseconds from the FreeRTOS tick.
 *
 * Suitable for latency and metrics-window calculations. Unlike
 * `gateway_ipc_now_ms()`, this value is not shifted by SNTP synchronisation.
 *
 * @return Milliseconds since scheduler start.
 */
uint32_t gateway_ipc_monotonic_ms(void);

/*******************************************************************************
 * Timestamp helper
 ******************************************************************************/

/**
 * @brief Get current wall-clock timestamp in milliseconds.
 *
 * Delegates to wallclock_now_ms():
 *   - After SNTP sync: Unix epoch milliseconds (UTC).
 *   - Before sync: milliseconds since FreeRTOS scheduler start.
 *
 * Suitable for ts_ms fields in telemetry/status/event records.
 *
 * @return Milliseconds (epoch or uptime - call wallclock_is_synced()
 *         to distinguish).
 */
uint64_t gateway_ipc_now_ms(void);

/*******************************************************************************
 * Status posting convenience
 ******************************************************************************/

/**
 * @brief Post a status record to the status queue (non-blocking, best-effort).
 *
 * If the queue is full, the function falls back to the status rescue ring.
 * Returns true if the record was accepted by either path, false if both are
 * full and the record is dropped.
 */
bool gateway_ipc_try_post_status(const status_record_t *rec);

/*******************************************************************************
 * Event posting convenience
 ******************************************************************************/

/**
 * @brief Post an event to the event queue (non-blocking, best-effort).
 *
 * If the queue is full, the function falls back to the event rescue ring.
 * The event is dropped only if both the queue and rescue ring are full.
 *
 * @param[in] type    Event type.
 * @param[in] detail  Detail string (truncated to EVT_DETAIL_MAX-1).
 */
void gateway_ipc_post_event(event_type_t type, const char *detail);

/*******************************************************************************
 * MQTT task notification (for command response wake-up)
 ******************************************************************************/

/**
 * @brief Register the MQTT task handle for notification-based wakeups.
 *
 * Called once from mqtt_gw_task at startup.
 */
void gateway_ipc_register_mqtt_task(TaskHandle_t handle);

/**
 * @brief Wake the MQTT task via xTaskNotifyGive.
 *
 * Called from:
 *   - MQTT callback after enqueueing a raw command
 *   - pmbus_poll_task after enqueueing a command response
 *
 * Safe to call when no task is registered (no-op).
 */
void gateway_ipc_notify_mqtt_task(void);

/** @} */  /* end of gateway_ipc */
