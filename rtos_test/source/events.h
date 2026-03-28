/**
 * @file events.h
 * @brief Gateway event record type and JSON encoding.
 * @ingroup events
 *
 * @details
 * Events are one-shot notifications about state changes, published to:
 * `pmbus/gw01/events` (topic built from `g_config.mqtt.base_topic`)
 *
 * Event types (MVP):
 *   - MQTT_CONNECTED / MQTT_DISCONNECTED
 *   - PMBUS_DEVICE_OFFLINE / PMBUS_DEVICE_ONLINE
 *   - PMBUS_BUS_RECOVERY / PMBUS_BUS_RECOVERY_FAILED
 *   - I2C_CONTROLLER_RESET (D1-2: SCB disable/re-enable path)
 *   - BUFFER_OVERFLOW / QUEUE_OVERFLOW
 *
 * @see agent.md §4, docs/mqtt_topics.md §4.4
 *
 * @defgroup events Events
 * @brief Event types, records, and JSON encoding for state-change notifications.
 * @{
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*******************************************************************************
 * Event types
 ******************************************************************************/
typedef enum {
    EVT_MQTT_CONNECTED          = 0,
    EVT_MQTT_DISCONNECTED       = 1,
    EVT_PMBUS_DEVICE_OFFLINE    = 2,
    EVT_PMBUS_DEVICE_ONLINE     = 3,
    EVT_PMBUS_BUS_RECOVERY      = 4,
    EVT_PMBUS_BUS_RECOVERY_FAIL = 5,
    EVT_BUFFER_OVERFLOW         = 6,
    EVT_QUEUE_OVERFLOW          = 7,
    EVT_I2C_CONTROLLER_RESET    = 8,  /**< D1-2: SCB disable/re-enable path */
} event_type_t;

/*******************************************************************************
 * Event record
 ******************************************************************************/

/** Max length of the detail string (fixed-size for queue-ability) */
#define EVT_DETAIL_MAX  48u

typedef struct {
    uint64_t     ts_ms;                     /**< Wall-clock (epoch ms UTC
                                                 after SNTP; uptime-ms
                                                 before sync)               */
    bool         time_synced;               /**< true once SNTP has synced  */
    event_type_t type;                      /**< Event type enum            */
    char         detail[EVT_DETAIL_MAX];    /**< Human-readable detail      */
} event_record_t;

/*******************************************************************************
 * API
 ******************************************************************************/

/**
 * @brief Get the string name for an event type.
 *
 * @param[in]  type  Event type enum value
 * @return     Static string, e.g. "MQTT_CONNECTED".
 */
const char *event_type_str(event_type_t type);

/**
 * @brief Encode an event record into JSON.
 *
 * Produces: {"ts_ms":..., "type":"MQTT_DISCONNECTED", "detail":"wifi_lost"}
 *
 * @param[in]  evt     Pointer to event record
 * @param[out] out     Output buffer
 * @param[in]  out_sz  Buffer size
 *
 * @return Number of bytes written, or -1 if buffer too small.
 */
int encode_event_json(const event_record_t *evt, char *out, size_t out_sz);

/**
 * @brief Build the events topic string.
 *
 * Format: "<base_topic>/events"
 *
 * @param[out] out     Output buffer
 * @param[in]  out_sz  Buffer size
 *
 * @return Number of bytes written, or -1 if buffer too small.
 */
int build_events_topic(char *out, size_t out_sz);

/** @} */  /* end of events */
