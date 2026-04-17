/**
 * @file emergency_ring.c
 * @brief Rescue rings for telemetry, status, and event overflow paths.
 */
#include "emergency_ring.h"
#include "FreeRTOS.h"
#include "task.h"

static telemetry_record_t s_ring[EMERGENCY_RING_CAPACITY];
static uint32_t s_head = 0u;
static uint32_t s_tail = 0u;

static status_record_t s_status_ring[EMERGENCY_STATUS_RING_CAPACITY];
static uint32_t s_status_head = 0u;
static uint32_t s_status_tail = 0u;

static event_record_t s_event_ring[EMERGENCY_EVENT_RING_CAPACITY];
static uint32_t s_event_head = 0u;
static uint32_t s_event_tail = 0u;

void emergency_ring_init(void)
{
    taskENTER_CRITICAL();
    s_head = 0u;
    s_tail = 0u;
    s_status_head = 0u;
    s_status_tail = 0u;
    s_event_head = 0u;
    s_event_tail = 0u;
    taskEXIT_CRITICAL();
}

bool emergency_ring_put(const telemetry_record_t *rec)
{
    bool accepted = false;

    if (rec == NULL)
    {
        return false;
    }

    taskENTER_CRITICAL();
    uint32_t next_head = (s_head + 1u) % EMERGENCY_RING_CAPACITY;
    if (next_head != s_tail)
    {
        s_ring[s_head] = *rec;
        s_head = next_head;
        accepted = true;
    }
    taskEXIT_CRITICAL();

    return accepted;
}

bool emergency_ring_get(telemetry_record_t *out_rec)
{
    bool got_record = false;

    if (out_rec == NULL)
    {
        return false;
    }

    taskENTER_CRITICAL();
    if (s_head != s_tail)
    {
        *out_rec = s_ring[s_tail];
        s_tail = (s_tail + 1u) % EMERGENCY_RING_CAPACITY;
        got_record = true;
    }
    taskEXIT_CRITICAL();

    return got_record;
}

bool emergency_status_ring_put(const status_record_t *rec)
{
    bool accepted = false;

    if (rec == NULL)
    {
        return false;
    }

    taskENTER_CRITICAL();
    uint32_t next_head = (s_status_head + 1u) % EMERGENCY_STATUS_RING_CAPACITY;
    if (next_head != s_status_tail)
    {
        s_status_ring[s_status_head] = *rec;
        s_status_head = next_head;
        accepted = true;
    }
    taskEXIT_CRITICAL();

    return accepted;
}

bool emergency_status_ring_get(status_record_t *out_rec)
{
    bool got_record = false;

    if (out_rec == NULL)
    {
        return false;
    }

    taskENTER_CRITICAL();
    if (s_status_head != s_status_tail)
    {
        *out_rec = s_status_ring[s_status_tail];
        s_status_tail = (s_status_tail + 1u) % EMERGENCY_STATUS_RING_CAPACITY;
        got_record = true;
    }
    taskEXIT_CRITICAL();

    return got_record;
}

bool emergency_event_ring_put(const event_record_t *evt)
{
    bool accepted = false;

    if (evt == NULL)
    {
        return false;
    }

    taskENTER_CRITICAL();
    uint32_t next_head = (s_event_head + 1u) % EMERGENCY_EVENT_RING_CAPACITY;
    if (next_head != s_event_tail)
    {
        s_event_ring[s_event_head] = *evt;
        s_event_head = next_head;
        accepted = true;
    }
    taskEXIT_CRITICAL();

    return accepted;
}

bool emergency_event_ring_get(event_record_t *out_evt)
{
    bool got_record = false;

    if (out_evt == NULL)
    {
        return false;
    }

    taskENTER_CRITICAL();
    if (s_event_head != s_event_tail)
    {
        *out_evt = s_event_ring[s_event_tail];
        s_event_tail = (s_event_tail + 1u) % EMERGENCY_EVENT_RING_CAPACITY;
        got_record = true;
    }
    taskEXIT_CRITICAL();

    return got_record;
}
