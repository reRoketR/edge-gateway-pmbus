/**
 * @file pmbus_poll_task.h
 * @brief PMBus polling task (Task A).
 * @ingroup pmbus_poll_task
 *
 * @details
 * Timer-driven polling of all configured PMBus devices.  For each device:
 *   - Reads all telemetry commands (VIN, VOUT, IIN, IOUT, TEMP, POUT)
 *   - Decodes raw values via pmbus_decode (Linear11/Linear16)
 *   - Builds a telemetry_record_t and pushes to telemetry_queue
 *   - Periodically reads status registers and pushes to status_queue
 *   - Emits events for device online/offline transitions
 *   - Updates metrics counters
 *
 * @see agent.md §6 (Task A), §3 (PMBus commands)
 *
 * @defgroup pmbus_poll_task PMBus Poll Task
 * @brief Task A — periodic I²C polling of PMBus targets.
 * @{
 */

#pragma once

#include <stdint.h>

/*******************************************************************************
 * Task parameters
 ******************************************************************************/
#define PMBUS_POLL_TASK_STACK_SIZE   (1024u)
#define PMBUS_POLL_TASK_PRIORITY    (4u)    /* High-ish; above mqtt_task */

/*******************************************************************************
 * API
 ******************************************************************************/

/**
 * @brief FreeRTOS task function for PMBus polling.
 *
 * Initialises the PMBus master driver, then loops forever:
 *   - For each device, if poll_period_ms has elapsed, read telemetry commands
 *   - For each device, if status_period_ms has elapsed, read status registers
 *   - Update metrics counters
 *   - Sleep until next poll deadline
 *
 * @param[in] pvParameters  Unused
 */
void pmbus_poll_task(void *pvParameters);

/** @} */  /* end of pmbus_poll_task */
