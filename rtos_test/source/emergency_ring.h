/**
 * @file emergency_ring.h
 * @brief Small rescue rings for records that miss the primary IPC queues.
 *
 * @details
 * These rings are a last-chance RAM tier that sits in front of `buffer_task`.
 * They are used only when the normal FreeRTOS queue for a given record class
 * is already full:
 *   - telemetry overflow from `pmbus_poll_task`
 *   - status overflow from `pmbus_poll_task`
 *   - event overflow from `gateway_ipc_post_event()`
 *
 * `buffer_task` drains the normal queues first and the rescue rings second so
 * that queue-resident older records are admitted before newer rescue records.
 */
#ifndef EMERGENCY_RING_H
#define EMERGENCY_RING_H

#include <stdbool.h>
#include <stdint.h>
#include "telemetry.h"
#include "events.h"

/** Capacity of the telemetry rescue ring (256 records = ~6 KB). */
#define EMERGENCY_RING_CAPACITY         256u

/** Capacity of the status rescue ring. */
#define EMERGENCY_STATUS_RING_CAPACITY   32u

/** Capacity of the event rescue ring. */
#define EMERGENCY_EVENT_RING_CAPACITY    32u

/**
 * @brief Initialize all rescue ring pointers.
 */
void emergency_ring_init(void);

/**
 * @brief Put a telemetry record into the telemetry rescue ring.
 * @param rec Pointer to the telemetry record to save.
 * @return true if saved, false if the rescue ring is also full.
 */
bool emergency_ring_put(const telemetry_record_t *rec);

/**
 * @brief Get a telemetry record from the telemetry rescue ring.
 * @param out_rec Pointer to the structure where the record will be copied.
 * @return true if a record was retrieved, false if the ring is empty.
 */
bool emergency_ring_get(telemetry_record_t *out_rec);

/**
 * @brief Put a status record into the status rescue ring.
 */
bool emergency_status_ring_put(const status_record_t *rec);

/**
 * @brief Get a status record from the status rescue ring.
 */
bool emergency_status_ring_get(status_record_t *out_rec);

/**
 * @brief Put an event record into the event rescue ring.
 */
bool emergency_event_ring_put(const event_record_t *evt);

/**
 * @brief Get an event record from the event rescue ring.
 */
bool emergency_event_ring_get(event_record_t *out_evt);

#endif /* EMERGENCY_RING_H */
