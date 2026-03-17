/**
 * @file metrics.h
 * @brief Gateway metrics collection and JSON encoding.
 * @ingroup metrics
 *
 * @details
 * Three categories of metrics:
 *   1. **Delta counters** — reset after each metrics publish
 *   2. **Gauges** — point-in-time values (buffer depth, RSSI, uptime)
 *   3. **Timing** — ring buffer of latency samples for avg/p95/max
 *
 * The metrics module uses a single-writer pattern: only one task
 * calls the increment functions, so no mutex is required for counters.
 * The publish function atomically snapshots and resets counters.
 *
 * @see agent.md §7, docs/mqtt_topics.md §4.3
 *
 * @defgroup metrics Metrics
 * @brief Performance counters, gauges, timing ring buffer, and JSON encoding.
 * @{
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/*******************************************************************************
 * Configuration
 ******************************************************************************/

/** Ring buffer size for latency samples (current firmware uses 50 samples) */
#define METRICS_LATENCY_RING_SIZE   50u

/*******************************************************************************
 * Delta counters (reset after each metrics publish)
 ******************************************************************************/
typedef struct {
    uint32_t pmbus_reads_ok;
    uint32_t pmbus_reads_fail;
    uint32_t pmbus_retries;
    uint32_t pmbus_timeouts;
    uint32_t pmbus_nack;
    uint32_t pmbus_crc_pec_fail;
    uint32_t mqtt_pub_ok;
    uint32_t mqtt_pub_fail;
    uint32_t mqtt_reconnects;
    uint32_t buffer_enqueued;
    uint32_t buffer_dequeued;
    uint32_t buffer_dropped;
    uint32_t queue_drops;           /**< Records lost due to full FreeRTOS queue */
    uint32_t telemetry_enqueued;    /**< Telemetry records successfully enqueued  */
} metrics_counters_t;

/*******************************************************************************
 * Gauges (point-in-time values, not reset)
 ******************************************************************************/
typedef struct {
    uint32_t buffer_depth_ram;
    uint32_t buffer_depth_flash;
    uint32_t telemetry_queue_depth;
    int32_t  wifi_rssi_dbm;
    uint32_t uptime_s;
} metrics_gauges_t;

/*******************************************************************************
 * Timing statistics (computed from ring buffer)
 ******************************************************************************/
typedef struct {
    /* read_to_publish = PMBus read start → MQTT publish complete */
    uint32_t read_to_publish_avg_us;    /**< Average in microseconds          */
    uint32_t read_to_publish_p95_us;    /**< 95th percentile in microseconds  */
    uint32_t read_to_publish_max_us;    /**< Maximum in microseconds          */

    /* PMBus transaction time only */
    uint32_t pmbus_txn_avg_us;
    uint32_t pmbus_txn_max_us;

    /* MQTT publish time only */
    uint32_t mqtt_publish_avg_us;
    uint32_t mqtt_publish_max_us;
} metrics_timing_t;

/*******************************************************************************
 * Rates (computed from counters + window)
 ******************************************************************************/
typedef struct {
    uint32_t telemetry_msgs_per_s_x10;  /**< ×10 to avoid float: 490 = 49.0  */
    uint32_t pmbus_cmds_per_s_x10;      /**< ×10 to avoid float              */
} metrics_rates_t;

/*******************************************************************************
 * Complete metrics snapshot (for JSON encoding)
 ******************************************************************************/
typedef struct {
    uint64_t          ts_ms;
    uint32_t          window_ms;
    metrics_counters_t counters;
    metrics_gauges_t   gauges;
    metrics_timing_t   timing;
    metrics_rates_t    rates;
} metrics_snapshot_t;

/*******************************************************************************
 * Initialization
 ******************************************************************************/

/**
 * @brief Initialize the metrics module. Zeroes all counters and ring buffers.
 */
void metrics_init(void);

/*******************************************************************************
 * Counter increment functions (called from various tasks)
 *
 * Most counters follow a single-writer pattern (one task per counter),
 * so bare ++ is safe on Cortex-M4 without additional synchronisation.
 *
 * Exception: `metrics_inc_queue_drops()` may be called from both
 * pmbus_poll_task and mqtt_gw_task, so it uses a critical section.
 *
 * The snapshot/reset path (`metrics_snapshot_and_reset`) already uses a
 * critical section for the copy + clear, so no counter update is lost
 * across a window boundary.
 *****************************************************************************/

void metrics_inc_pmbus_reads_ok(void);
void metrics_inc_pmbus_reads_fail(void);
void metrics_inc_pmbus_retries(void);
void metrics_inc_pmbus_timeouts(void);
void metrics_inc_pmbus_nack(void);
void metrics_inc_pmbus_pec_fail(void);
void metrics_inc_mqtt_pub_ok(void);
void metrics_inc_mqtt_pub_fail(void);
void metrics_inc_mqtt_reconnects(void);
void metrics_inc_buffer_enqueued(void);
void metrics_inc_buffer_dequeued(void);
void metrics_inc_buffer_dropped(void);
void metrics_inc_queue_drops(void);
void metrics_inc_telemetry_enqueued(void);

/*******************************************************************************
 * Gauge setters (called from various tasks)
 ******************************************************************************/

void metrics_set_buffer_depth_ram(uint32_t depth);
void metrics_set_buffer_depth_flash(uint32_t depth);
void metrics_set_telemetry_queue_depth(uint32_t depth);
void metrics_set_wifi_rssi(int32_t rssi_dbm);

/*******************************************************************************
 * Timing sample recorders
 *
 * Call these to add a latency sample to the ring buffer.
 * The ring buffer wraps around, keeping the latest N samples.
 ******************************************************************************/

/**
 * @brief Record a read-to-publish latency sample (in microseconds).
 */
void metrics_record_read_to_publish_us(uint32_t latency_us);

/**
 * @brief Record a PMBus transaction latency sample (in microseconds).
 */
void metrics_record_pmbus_txn_us(uint32_t latency_us);

/**
 * @brief Record an MQTT publish latency sample (in microseconds).
 */
void metrics_record_mqtt_publish_us(uint32_t latency_us);

/*******************************************************************************
 * Snapshot & reset
 ******************************************************************************/

/**
 * @brief Take a snapshot of all metrics, reset delta counters, and compute
 *        timing statistics from the ring buffers.
 *
 * This is called by the metrics publish cycle. After calling this function,
 * delta counters are reset to zero. Ring buffers are NOT cleared (they
 * continue to accumulate for the next window).
 *
 * @param[out] snap             Pointer to snapshot struct to fill
 * @param[in]  ts_ms            Current wall-clock timestamp for JSON `ts_ms`
 * @param[in]  now_monotonic_ms Current monotonic time in ms for `window_ms`
 *                              and uptime calculations
 */
void metrics_snapshot_and_reset(metrics_snapshot_t *snap,
                                uint64_t ts_ms,
                                uint64_t now_monotonic_ms);

/*******************************************************************************
 * JSON encoding
 ******************************************************************************/

/**
 * @brief Encode a metrics snapshot into the MQTT JSON payload.
 *
 * Produces the exact schema from mqtt_topics.md §4.3.
 *
 * @param[in]  snap    Pointer to metrics snapshot
 * @param[out] out     Output buffer
 * @param[in]  out_sz  Buffer size
 *
 * @return Number of bytes written, or -1 if buffer too small.
 */
int encode_metrics_json(const metrics_snapshot_t *snap,
                        char *out, size_t out_sz);

/**
 * @brief Build the metrics topic string.
 *
 * Format: "<base_topic>/metrics"
 *
 * @param[out] out     Output buffer
 * @param[in]  out_sz  Buffer size
 *
 * @return Number of bytes written, or -1 if buffer too small.
 */
int build_metrics_topic(char *out, size_t out_sz);

/** @} */  /* end of metrics */
