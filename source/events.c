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

/**
 * @brief Format uint64_t as decimal string (portable, avoids %llu on MinGW).
 */
static int fmt_u64(char *buf, size_t sz, uint64_t val)
{
    if (val == 0u) return snprintf(buf, sz, "0");
    char tmp[21];
    int p = (int)sizeof(tmp) - 1;
    tmp[p] = '\0';
    while (val > 0u && p > 0) { p--; tmp[p] = (char)('0' + (int)(val % 10u)); val /= 10u; }
    return snprintf(buf, sz, "%s", &tmp[p]);
}

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
        "{\"ts_ms\":%s,\"type\":\"%s\",\"detail\":\"%s\"}",
        ts_buf,
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
