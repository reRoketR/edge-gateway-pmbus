/*******************************************************************************
 * File Name:   emergency_ring.h
 *
 * Description: Emergency RAM ring buffer for telemetry records.
 *              Acts as a lock-free Single-Producer Single-Consumer (SPSC)
 *              circular buffer to rescue telemetry during transition zones
 *              when the main FreeRTOS IPC queue is full.
 *
 ******************************************************************************/
#ifndef EMERGENCY_RING_H
#define EMERGENCY_RING_H

#include <stdbool.h>
#include <stdint.h>
#include "telemetry.h"

/** Capacity of the emergency ring (256 records = ~6 KB) */
#define EMERGENCY_RING_CAPACITY 256u

/**
 * @brief Initialize the ring buffer pointers.
 */
void emergency_ring_init(void);

/**
 * @brief Put a telemetry record into the emergency ring.
 *        Called from pmbus_poll_task when the IPC queue is full.
 * @param rec Pointer to the telemetry record to save.
 * @return true if saved, false if the emergency ring is also full.
 */
bool emergency_ring_put(const telemetry_record_t *rec);

/**
 * @brief Get a telemetry record from the emergency ring.
 *        Called from mqtt_gw_task to drain the rescued records.
 * @param out_rec Pointer to the structure where the record will be copied.
 * @return true if a record was retrieved, false if the ring is empty.
 */
bool emergency_ring_get(telemetry_record_t *out_rec);

#endif /* EMERGENCY_RING_H */
