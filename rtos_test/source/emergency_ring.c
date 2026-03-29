/*******************************************************************************
 * File Name:   emergency_ring.c
 *
 * Description: Implementation of the emergency RAM ring buffer.
 *
 ******************************************************************************/
#include "emergency_ring.h"

static telemetry_record_t s_ring[EMERGENCY_RING_CAPACITY];
static volatile uint32_t s_head = 0; /* Written by pmbus_poll_task */
static volatile uint32_t s_tail = 0; /* Read by mqtt_gw_task */

void emergency_ring_init(void)
{
    s_head = 0;
    s_tail = 0;
}

bool emergency_ring_put(const telemetry_record_t *rec)
{
    uint32_t next_head = (s_head + 1u) % EMERGENCY_RING_CAPACITY;
    if (next_head == s_tail)
    {
        return false; /* Ring is full */
    }
    s_ring[s_head] = *rec;
    s_head = next_head;
    return true;
}

bool emergency_ring_get(telemetry_record_t *out_rec)
{
    if (s_head == s_tail)
    {
        return false; /* Ring is empty */
    }
    *out_rec = s_ring[s_tail];
    s_tail = (s_tail + 1u) % EMERGENCY_RING_CAPACITY;
    return true;
}
