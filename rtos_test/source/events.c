/*******************************************************************************
 * File Name:   events.c
 *
 * Description: Event type string mapping and JSON encoding.
 *
 * Related Document: agent.md §4, docs/mqtt_topics.md §4.4
 *
 ******************************************************************************/

/* Enable C99-compliant printf on MinGW (for %llu support in host tests) */
#if defined(__MINGW32__) || defined(__MINGW64__)
#define __USE_MINGW_ANSI_STDIO 1
#endif

#include "events.h"
#include "gateway_config.h"
#include "gw_util.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

/*******************************************************************************
 * Event type → string mapping
 ******************************************************************************/
static const char *event_type_names[] = {
    [EVT_MQTT_CONNECTED]          = "MQTT_CONNECTED",
    [EVT_MQTT_DISCONNECTED]       = "MQTT_DISCONNECTED",
    [EVT_PMBUS_DEVICE_OFFLINE]    = "PMBUS_DEVICE_OFFLINE",
    [EVT_PMBUS_DEVICE_ONLINE]     = "PMBUS_DEVICE_ONLINE",
    [EVT_PMBUS_BUS_RECOVERY]      = "PMBUS_BUS_RECOVERY",
    [EVT_PMBUS_BUS_RECOVERY_FAIL] = "PMBUS_BUS_RECOVERY_FAILED",
    [EVT_BUFFER_OVERFLOW]         = "BUFFER_OVERFLOW",
    [EVT_QUEUE_OVERFLOW]          = "QUEUE_OVERFLOW",
};

#define NUM_EVENT_TYPES (sizeof(event_type_names) / sizeof(event_type_names[0]))

const char *event_type_str(event_type_t type)
{
    if ((unsigned)type < NUM_EVENT_TYPES && event_type_names[type] != NULL)
    {
        return event_type_names[type];
    }
    return "UNKNOWN";
}

/* fmt_u64() is now provided by gw_util.h */

/*******************************************************************************
 * JSON encoding
 ******************************************************************************/
int encode_event_json(const event_record_t *evt, char *out, size_t out_sz)
{
    if (evt == NULL || out == NULL || out_sz < 32u)
    {
        return -1;
    }

    char ts_buf[24];
    fmt_u64(ts_buf, sizeof(ts_buf), evt->ts_ms);

    int len = snprintf(out, out_sz,
        "{\"ts_ms\":%s,\"time_synced\":%s,\"type\":\"%s\",\"detail\":\"%s\"}",
        ts_buf,
        evt->time_synced ? "true" : "false",
        event_type_str(evt->type),
        evt->detail);

    if (len < 0 || (size_t)len >= out_sz)
    {
        return -1;
    }

    return len;
}

/*******************************************************************************
 * Topic builder
 ******************************************************************************/
int build_events_topic(char *out, size_t out_sz)
{
    if (out == NULL || out_sz < 8u)
    {
        return -1;
    }

    int len = snprintf(out, out_sz, "%s/events", g_config.mqtt.base_topic);

    if (len < 0 || (size_t)len >= out_sz)
    {
        return -1;
    }

    return len;
}
