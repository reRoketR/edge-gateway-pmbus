/**
 * @file telemetry.h
 * @brief Fixed-size record structs for PMBus telemetry and status snapshots.
 * @ingroup telemetry
 *
 * @details
 * Defines:
 *   - telemetry_record_t — one complete read cycle of a PMBus device
 *   - status_record_t    — status registers snapshot
 *   - JSON encoding functions for both record types
 *
 * All encoding uses snprintf into caller-supplied buffers. No heap allocation.
 *
 * @see agent.md §4, §7; docs/mqtt_topics.md §4.1, §4.2
 *
 * @defgroup telemetry Telemetry
 * @brief Telemetry/status record structures, PMBus command codes, and JSON encoding.
 * @{
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*******************************************************************************
 * PMBus command codes used by the polling cycle
 ******************************************************************************/
#define PMBUS_CMD_READ_VIN              0x88u
#define PMBUS_CMD_READ_VOUT             0x8Bu
#define PMBUS_CMD_READ_IIN              0x89u
#define PMBUS_CMD_READ_IOUT             0x8Cu
#define PMBUS_CMD_READ_TEMPERATURE_1    0x8Du
#define PMBUS_CMD_READ_POUT             0x96u
#define PMBUS_CMD_STATUS_WORD           0x79u
#define PMBUS_CMD_STATUS_VOUT           0x7Au
#define PMBUS_CMD_STATUS_IOUT           0x7Bu
#define PMBUS_CMD_STATUS_TEMPERATURE    0x7Du
#define PMBUS_CMD_VOUT_MODE             0x20u
#define PMBUS_CMD_CLEAR_FAULTS          0x03u

/*******************************************************************************
 * Telemetry record (one PMBus read cycle per device)
 ******************************************************************************/

/** Number of telemetry values we read */
#define TELEM_NUM_COMMANDS  6u

typedef struct {
    /* --- Metadata --- */
    uint64_t    ts_ms;              /**< Wall-clock timestamp (epoch ms UTC
                                         after SNTP; uptime-ms before sync)    */
    uint32_t    read_start_ms;      /**< Monotonic read-start time in ms;
                                         internal only, not serialized         */
    bool        time_synced;        /**< true once SNTP has synchronised        */
    uint32_t    seq;                /**< Global sequence number                 */
    uint8_t     addr_7bit;          /**< 7-bit PMBus address                    */
    const char *label;              /**< Device label from config               */
    bool        pec;                /**< PEC was enabled for this read          */
    uint16_t    read_ms;            /**< Duration of PMBus read cycle (ms)      */
    uint8_t     retries;            /**< Total retries across all commands      */

    /* --- Decoded SI values (milli-units to avoid FPU) --- */
    int32_t     vin_mV;             /**< READ_VIN  in millivolts (Linear11)     */
    uint32_t    vout_mV;            /**< READ_VOUT in millivolts (Linear16)     */
    int32_t     iin_mA;             /**< READ_IIN  in milliamps  (Linear11)     */
    int32_t     iout_mA;            /**< READ_IOUT in milliamps  (Linear11)     */
    int32_t     temp1_mC;           /**< READ_TEMPERATURE_1 in milli-°C         */
    int32_t     pout_mW;            /**< READ_POUT in milliwatts (Linear11)     */

    /* --- Raw hex words (for DEBUG/raw field in JSON) --- */
    uint16_t    raw_vin;
    uint16_t    raw_vout;
    uint16_t    raw_iin;
    uint16_t    raw_iout;
    uint16_t    raw_temp1;
    uint16_t    raw_pout;

    /* --- Validity flags (bit per command, set if read succeeded) --- */
    uint8_t     valid_mask;         /**< Bit 0=VIN, 1=VOUT, 2=IIN, 3=IOUT,
                                         4=TEMP1, 5=POUT                       */
} telemetry_record_t;

/** Validity bit positions */
#define TELEM_VALID_VIN     (1u << 0u)
#define TELEM_VALID_VOUT    (1u << 1u)
#define TELEM_VALID_IIN     (1u << 2u)
#define TELEM_VALID_IOUT    (1u << 3u)
#define TELEM_VALID_TEMP1   (1u << 4u)
#define TELEM_VALID_POUT    (1u << 5u)
#define TELEM_VALID_ALL     (0x3Fu)

/*******************************************************************************
 * Status record (status registers snapshot)
 ******************************************************************************/
typedef struct {
    uint64_t    ts_ms;              /**< Wall-clock (same semantics as above)   */
    bool        time_synced;        /**< true once SNTP has synchronised        */
    uint32_t    seq;
    uint8_t     addr_7bit;
    const char *label;

    uint16_t    status_word;
    uint8_t     status_vout;
    uint8_t     status_iout;
    uint8_t     status_temperature;

    /** Validity: each bit indicates if the corresponding read succeeded */
    uint8_t     valid_mask;
} status_record_t;

#define STATUS_VALID_WORD   (1u << 0u)
#define STATUS_VALID_VOUT   (1u << 1u)
#define STATUS_VALID_IOUT   (1u << 2u)
#define STATUS_VALID_TEMP   (1u << 3u)
#define STATUS_VALID_ALL    (0x0Fu)

/*******************************************************************************
 * JSON encoding — telemetry
 ******************************************************************************/

/**
 * @brief Encode a TelemetryRecord into the MQTT JSON payload.
 *
 * Produces the exact JSON schema from mqtt_topics.md §4.1:
 * {
 *   "ts_ms": ..., "seq": ..., "gw_id": "...", "addr": "0x58", "label": "...",
 *   "pec": true, "read_ms": 7, "retries": 0,
 *   "v": {"vin": 12.03, "vout": 1.02},
 *   "a": {"iin": 0.84, "iout": 5.10},
 *   "c": {"temp1": 42.5},
 *   "w": {"pout": 5.20},
 *   "raw": {"read_vout": "0x0123"}
 * }
 *
 * Values are converted from milli-units to SI floats (with 2-3 decimal digits).
 * Invalid readings (valid_mask bit clear) are omitted from the JSON.
 *
 * @param[in]  rec     Pointer to the telemetry record
 * @param[out] out     Output buffer for JSON string
 * @param[in]  out_sz  Size of output buffer
 *
 * @return Number of bytes written (excluding NUL), or -1 if buffer too small.
 */
int encode_telemetry_json(const telemetry_record_t *rec,
                          char *out, size_t out_sz);

/*******************************************************************************
 * JSON encoding — status
 ******************************************************************************/

/**
 * @brief Encode a StatusRecord into the MQTT JSON payload.
 *
 * Produces the JSON schema from mqtt_topics.md §4.2.
 *
 * @param[in]  rec     Pointer to the status record
 * @param[out] out     Output buffer for JSON string
 * @param[in]  out_sz  Size of output buffer
 *
 * @return Number of bytes written (excluding NUL), or -1 if buffer too small.
 */
int encode_status_json(const status_record_t *rec,
                       char *out, size_t out_sz);

/*******************************************************************************
 * MQTT topic builder
 ******************************************************************************/

/**
 * @brief Build the MQTT topic string for a per-device message.
 *
 * Format: "<base_topic>/dev/0x<addr>/<suffix>"
 * Example: "pmbus/gw01/dev/0x58/telemetry"
 *
 * @param[out] out       Output buffer
 * @param[in]  out_sz    Buffer size
 * @param[in]  addr_7bit 7-bit device address
 * @param[in]  suffix    "telemetry" or "status"
 *
 * @return Number of bytes written, or -1 if buffer too small.
 */
int build_device_topic(char *out, size_t out_sz,
                       uint8_t addr_7bit, const char *suffix);

/** @} */  /* end of telemetry */
