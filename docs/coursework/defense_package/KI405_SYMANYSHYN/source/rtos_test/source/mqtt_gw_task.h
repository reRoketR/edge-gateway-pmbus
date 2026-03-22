/**
 * @file mqtt_gw_task.h
 * @brief MQTT gateway task (Task B).
 * @ingroup mqtt_gw_task
 *
 * @details
 * Responsibilities:
 *   - Connect Wi-Fi and initialise MQTT client
 *   - Reconnect with exponential backoff on disconnect
 *   - Consume telemetry/status/event queues and publish JSON
 *   - On publish failure: enqueue to buffer_mgr
 *   - Periodically publish metrics
 *   - Maintain MQTT online/offline flag in gateway_ipc
 *
 * @see agent.md §6 (Task B), §4
 *
 * @defgroup mqtt_gw_task MQTT Gateway Task
 * @brief Task B — Wi-Fi/MQTT connection, JSON publish, and offline fallback.
 * @{
 */

#pragma once

#include <stdint.h>

/*******************************************************************************
 * Task parameters
 ******************************************************************************/
#define MQTT_GW_TASK_STACK_SIZE     (1024u * 3u)
#define MQTT_GW_TASK_PRIORITY       (3u)

/*******************************************************************************
 * API
 ******************************************************************************/

/**
 * @brief FreeRTOS task function for the MQTT gateway.
 *
 * @param[in] pvParameters  Unused
 */
void mqtt_gw_task(void *pvParameters);

/** @} */  /* end of mqtt_gw_task */
