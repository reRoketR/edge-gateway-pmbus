/*******************************************************************************
 * File Name:   metrics.c
 *
 * Description: Metrics collection, ring buffer statistics, JSON encoding.
 *
 *              Architecture:
 *                - Delta counters: critical-section protected increments,
 *                  reset on snapshot
 *                - Gauges: written by setter, read on snapshot
 *                - Timing: 3 ring buffers (read-to-pub, pmbus txn, mqtt pub)
 *                  Each holds METRICS_LATENCY_RING_SIZE samples.
 *                  On snapshot: copy → sort → extract avg/p95/max.
 *
 * Related Document: agent.md §7, docs/mqtt_topics.md §4.3
 *
 ******************************************************************************/

/* Enable C99-compliant printf on MinGW (for %llu support in host tests) */
#if defined(__MINGW32__) || defined(__MINGW64__)
#define __USE_MINGW_ANSI_STDIO 1
#endif

#include "metrics.h"
#include "telemetry.h"
#include "gateway_config.h"
#include "gw_util.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

/*******************************************************************************
 * Private data
 ******************************************************************************/

/** Delta counters — reset after each snapshot */
static volatile metrics_counters_t s_counters;

/** Gauges — point-in-time, not reset */
static volatile metrics_gauges_t s_gauges;

/** Boot time (set once in metrics_init, used for uptime) */
static uint64_t s_boot_ms;

/** Last snapshot time from the monotonic clock (for window_ms calculation) */
static uint64_t s_last_snapshot_monotonic_ms;

/*******************************************************************************
 * Timing ring buffers
 ******************************************************************************/
typedef struct {
    uint32_t samples[METRICS_LATENCY_RING_SIZE];
    uint16_t head;      /**< Next write position */
    uint16_t count;     /**< Number of valid samples (≤ RING_SIZE) */
} latency_ring_t;

static latency_ring_t s_ring_read_to_pub;
static latency_ring_t s_ring_pmbus_txn;
static latency_ring_t s_ring_mqtt_pub;

static void ring_add(latency_ring_t *ring, uint32_t value)
{
    ring->samples[ring->head] = value;
    ring->head = (ring->head + 1u) % METRICS_LATENCY_RING_SIZE;
    if (ring->count < METRICS_LATENCY_RING_SIZE)
    {
        ring->count++;
    }
}

/*******************************************************************************
 * Sort helper (for p95 computation) — insertion sort, small N
 ******************************************************************************/
static void sort_u32(uint32_t *arr, uint16_t n)
{
    for (uint16_t i = 1u; i < n; i++)
    {
        uint32_t key = arr[i];
        int16_t j = (int16_t)i - 1;
        while (j >= 0 && arr[j] > key)
        {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

/**
 * @brief Compute avg, p95, max from a ring buffer.
 *
 * Uses a scratch buffer (copy of ring samples) for sorting.
 */
static void compute_stats(const latency_ring_t *ring,
                          uint32_t *out_avg, uint32_t *out_p95, uint32_t *out_max)
{
    *out_avg = 0u;
    *out_p95 = 0u;
    *out_max = 0u;

    if (ring->count == 0u)
    {
        return;
    }

    /* Copy valid samples to scratch buffer */
    uint32_t scratch[METRICS_LATENCY_RING_SIZE];
    uint16_t n = ring->count;
    memcpy(scratch, ring->samples, n * sizeof(uint32_t));

    /* Sort for percentile computation */
    sort_u32(scratch, n);

    /* Average */
    uint64_t sum = 0u;
    for (uint16_t i = 0u; i < n; i++)
    {
        sum += scratch[i];
    }
    *out_avg = (uint32_t)(sum / n);

    /* Max = last element after sort */
    *out_max = scratch[n - 1u];

    /* P95: index = ceil(0.95 * N) - 1 */
    uint16_t p95_idx = (uint16_t)(((uint32_t)n * 95u + 99u) / 100u);
    if (p95_idx > 0u) p95_idx--;
    if (p95_idx >= n) p95_idx = n - 1u;
    *out_p95 = scratch[p95_idx];
}

/*******************************************************************************
 * Initialization
 ******************************************************************************/
void metrics_init(void)
{
    memset((void *)&s_counters, 0, sizeof(s_counters));
    memset((void *)&s_gauges, 0, sizeof(s_gauges));
    memset(&s_ring_read_to_pub, 0, sizeof(s_ring_read_to_pub));
    memset(&s_ring_pmbus_txn, 0, sizeof(s_ring_pmbus_txn));
    memset(&s_ring_mqtt_pub, 0, sizeof(s_ring_mqtt_pub));

    s_boot_ms = 0u;
    s_last_snapshot_monotonic_ms = 0u;
}

/*******************************************************************************
 * Counter increments
 *
 * Atomicity policy:
 *   All delta counters use taskENTER/EXIT_CRITICAL around the increment.
 *   This is required because metrics_snapshot_and_reset() copies AND zeroes
 *   s_counters inside its own critical section.  Without matching protection
 *   on the increment side, an increment that lands between the copy and the
 *   memset would be silently lost.
 *
 *   In practice most counters have a single writer (the poll task or the
 *   MQTT task), so a bare ++ would usually be safe for the increment itself.
 *   The critical section is still needed to guard against the copy-then-reset
 *   race in the snapshot path.
 *
 *   Gauges are written by a single setter and only read (never reset) by the
 *   snapshot, so they do not need critical sections.
 ******************************************************************************/
static void counter_inc(volatile uint32_t *counter)
{
    taskENTER_CRITICAL();
    (*counter)++;
    taskEXIT_CRITICAL();
}

void metrics_inc_pmbus_reads_ok(void)    { counter_inc(&s_counters.pmbus_reads_ok);     }
void metrics_inc_pmbus_reads_fail(void)  { counter_inc(&s_counters.pmbus_reads_fail);   }
void metrics_inc_pmbus_retries(void)     { counter_inc(&s_counters.pmbus_retries);      }
void metrics_inc_pmbus_timeouts(void)    { counter_inc(&s_counters.pmbus_timeouts);     }
void metrics_inc_pmbus_nack(void)        { counter_inc(&s_counters.pmbus_nack);         }
void metrics_inc_pmbus_pec_fail(void)    { counter_inc(&s_counters.pmbus_crc_pec_fail); }
void metrics_inc_mqtt_pub_ok(void)       { counter_inc(&s_counters.mqtt_pub_ok);        }
void metrics_inc_mqtt_pub_fail(void)     { counter_inc(&s_counters.mqtt_pub_fail);      }
void metrics_inc_mqtt_reconnects(void)   { counter_inc(&s_counters.mqtt_reconnects);    }
void metrics_inc_buffer_enqueued(void)   { counter_inc(&s_counters.buffer_enqueued);    }
void metrics_inc_buffer_dequeued(void)   { counter_inc(&s_counters.buffer_dequeued);    }
void metrics_inc_buffer_dropped(void)    { counter_inc(&s_counters.buffer_dropped);     }
void metrics_inc_queue_drops(void)       { counter_inc(&s_counters.queue_drops);        }
void metrics_inc_telemetry_enqueued(void){ counter_inc(&s_counters.telemetry_enqueued); }
void metrics_inc_i2c_controller_resets(void) { counter_inc(&s_counters.i2c_controller_resets); }
void metrics_inc_i2c_bus_recoveries(void)   { counter_inc(&s_counters.i2c_bus_recoveries);   }
void metrics_inc_telemetry_suppressed(void) { counter_inc(&s_counters.telemetry_suppressed); }
void metrics_inc_status_suppressed(void)    { counter_inc(&s_counters.status_suppressed);    }

/*******************************************************************************
 * Gauge setters
 ******************************************************************************/
void metrics_set_buffer_depth_ram(uint32_t depth)   { s_gauges.buffer_depth_ram   = depth;    }
void metrics_set_buffer_depth_flash(uint32_t depth) { s_gauges.buffer_depth_flash = depth;    }
void metrics_set_telemetry_queue_depth(uint32_t depth)
{
    s_gauges.telemetry_queue_depth = depth;
}
void metrics_set_wifi_rssi(int32_t rssi_dbm)        { s_gauges.wifi_rssi_dbm      = rssi_dbm; }
void metrics_set_boot_count(uint32_t count)          { s_gauges.boot_count         = count;    }
void metrics_set_storage_total_writes(uint32_t n)    { s_gauges.storage_total_writes = n;      }
void metrics_set_storage_backend(uint8_t backend)    { s_gauges.storage_backend    = backend;  }

/*******************************************************************************
 * Timing sample recorders
 ******************************************************************************/
void metrics_record_read_to_publish_us(uint32_t us) { ring_add(&s_ring_read_to_pub, us); }
void metrics_record_pmbus_txn_us(uint32_t us)       { ring_add(&s_ring_pmbus_txn, us);   }
void metrics_record_mqtt_publish_us(uint32_t us)     { ring_add(&s_ring_mqtt_pub, us);    }

/*******************************************************************************
 * Snapshot & reset
 ******************************************************************************/
void metrics_snapshot_and_reset(metrics_snapshot_t *snap,
                                uint64_t ts_ms,
                                uint64_t now_monotonic_ms)
{
    if (snap == NULL) return;

    /* Set boot time on first call */
    if (s_boot_ms == 0u)
    {
        s_boot_ms = now_monotonic_ms;
    }

    /* Window calculation */
    uint32_t window_ms = 0u;
    if (s_last_snapshot_monotonic_ms > 0u)
    {
        window_ms = (uint32_t)(now_monotonic_ms -
                               s_last_snapshot_monotonic_ms);
    }
    /* else: first snapshot — window_ms stays 0; counters accumulated since
     * boot are reported but rates will be zero.  Post-processing scripts
     * should filter records where window_ms == 0. */

    snap->ts_ms     = ts_ms;
    snap->window_ms = window_ms;

    /* --- Critical section: copy + reset counters atomically ---
     * Without this, a counter increment between the memcpy and memset
     * would be lost.  The section is short (~dozen word copies). */
    taskENTER_CRITICAL();
    snap->counters = *(const metrics_counters_t *)&s_counters;
    memset((void *)&s_counters, 0, sizeof(s_counters));
    taskEXIT_CRITICAL();

    /* Copy gauges (not reset) */
    snap->gauges = *(const metrics_gauges_t *)&s_gauges;
    snap->gauges.uptime_s = (uint32_t)((now_monotonic_ms - s_boot_ms) / 1000u);

    /* Compute timing stats from ring buffers */
    compute_stats(&s_ring_read_to_pub,
                  &snap->timing.read_to_publish_avg_us,
                  &snap->timing.read_to_publish_p95_us,
                  &snap->timing.read_to_publish_max_us);

    {
        uint32_t dummy_p95;
        compute_stats(&s_ring_pmbus_txn,
                      &snap->timing.pmbus_txn_avg_us,
                      &dummy_p95,
                      &snap->timing.pmbus_txn_max_us);
        compute_stats(&s_ring_mqtt_pub,
                      &snap->timing.mqtt_publish_avg_us,
                      &dummy_p95,
                      &snap->timing.mqtt_publish_max_us);
    }

    /* Compute rates.
     *
     * telemetry_enqueued counts actual telemetry records pushed to the
     * FreeRTOS queue (1 per successful poll cycle, regardless of how many
     * individual PMBus commands succeeded within that cycle).
     *
     * pmbus_cmds_per_s uses the raw per-command counters as before.
     *
     * We multiply by 10000 (not 1000) because the stored value is ×10 to give
     * one decimal of precision without float.
     */
    if (window_ms > 0u)
    {
        uint32_t total_cmds = snap->counters.pmbus_reads_ok +
                              snap->counters.pmbus_reads_fail;

        /* Telemetry msgs/s = actually enqueued records / window */
        snap->rates.telemetry_msgs_per_s_x10 =
            (snap->counters.telemetry_enqueued * 10000u) / window_ms;

        /* PMBus bus commands per second (already per-command counts) */
        snap->rates.pmbus_cmds_per_s_x10 =
            (total_cmds * 10000u) / window_ms;
    }
    else
    {
        snap->rates.telemetry_msgs_per_s_x10 = 0u;
        snap->rates.pmbus_cmds_per_s_x10 = 0u;
    }

    s_last_snapshot_monotonic_ms = now_monotonic_ms;
}

/*******************************************************************************
 * JSON encoding
 ******************************************************************************/

/**
 * @brief Format microseconds as milliseconds with 1 decimal, e.g. 18200 → "18.2"
 */
static int fmt_us_to_ms_1dp(char *buf, size_t sz, uint32_t us)
{
    uint32_t ms_int  = us / 1000u;
    uint32_t ms_frac = (us % 1000u) / 100u;  /* 1 decimal place */
    return snprintf(buf, sz, "%u.%u", (unsigned)ms_int, (unsigned)ms_frac);
}

/* fmt_u64() is now provided by gw_util.h */

int encode_metrics_json(const metrics_snapshot_t *snap,
                        char *out, size_t out_sz)
{
    if (snap == NULL || out == NULL || out_sz < 128u)
    {
        return -1;
    }

    char *pos = out;
    const char *end = out + out_sz;
    char t[16];
    char ts_buf[24];

    fmt_u64(ts_buf, sizeof(ts_buf), snap->ts_ms);

    /* Helper macro for bounded printf */
    #define M_PRINTF(fmt, ...) do {                                  \
        int avail = (int)(end - pos);                                \
        if (avail <= 0) return -1;                                   \
        int w = snprintf(pos, (size_t)avail, fmt, ##__VA_ARGS__);    \
        if (w < 0 || w >= avail) return -1;                          \
        pos += w;                                                    \
    } while(0)

    /* Open + ts + window */
    M_PRINTF("{\"ts_ms\":%s,\"window_ms\":%u",
             ts_buf,
             (unsigned)snap->window_ms);

    /* counters_delta */
    M_PRINTF(",\"counters_delta\":{"
             "\"pmbus_reads_ok\":%u,"
             "\"pmbus_reads_fail\":%u,"
             "\"pmbus_retries\":%u,"
             "\"pmbus_timeouts\":%u,"
             "\"pmbus_nack\":%u,"
             "\"pmbus_crc_pec_fail\":%u,"
             "\"mqtt_pub_ok\":%u,"
             "\"mqtt_pub_fail\":%u,"
             "\"mqtt_reconnects\":%u,"
             "\"buffer_enqueued\":%u,"
             "\"buffer_dequeued\":%u,"
             "\"buffer_dropped\":%u,"
             "\"queue_drops\":%u,"
             "\"telemetry_enqueued\":%u,"
             "\"i2c_controller_resets\":%u,"
             "\"i2c_bus_recoveries\":%u,"
             "\"telemetry_suppressed\":%u,"
             "\"status_suppressed\":%u}",
             (unsigned)snap->counters.pmbus_reads_ok,
             (unsigned)snap->counters.pmbus_reads_fail,
             (unsigned)snap->counters.pmbus_retries,
             (unsigned)snap->counters.pmbus_timeouts,
             (unsigned)snap->counters.pmbus_nack,
             (unsigned)snap->counters.pmbus_crc_pec_fail,
             (unsigned)snap->counters.mqtt_pub_ok,
             (unsigned)snap->counters.mqtt_pub_fail,
             (unsigned)snap->counters.mqtt_reconnects,
             (unsigned)snap->counters.buffer_enqueued,
             (unsigned)snap->counters.buffer_dequeued,
             (unsigned)snap->counters.buffer_dropped,
             (unsigned)snap->counters.queue_drops,
             (unsigned)snap->counters.telemetry_enqueued,
             (unsigned)snap->counters.i2c_controller_resets,
             (unsigned)snap->counters.i2c_bus_recoveries,
             (unsigned)snap->counters.telemetry_suppressed,
             (unsigned)snap->counters.status_suppressed);

    /* gauges */
    M_PRINTF(",\"gauges\":{"
             "\"buffer_depth_ram\":%u,"
             "\"buffer_depth_flash\":%u,"
             "\"telemetry_queue_depth\":%u,"
             "\"wifi_rssi_dbm\":%d,"
             "\"uptime_s\":%u,"
             "\"boot_count\":%u,"
             "\"storage\":{\"backend\":\"%s\",\"total_writes\":%u}}",
             (unsigned)snap->gauges.buffer_depth_ram,
             (unsigned)snap->gauges.buffer_depth_flash,
             (unsigned)snap->gauges.telemetry_queue_depth,
             (int)snap->gauges.wifi_rssi_dbm,
             (unsigned)snap->gauges.uptime_s,
             (unsigned)snap->gauges.boot_count,
             (snap->gauges.storage_backend == 1u) ? "qspi" : "eeprom",
             (unsigned)snap->gauges.storage_total_writes);

    /* timing_ms (values stored in us, output in ms with 1 decimal) */
    M_PRINTF(",\"timing_ms\":{");

    fmt_us_to_ms_1dp(t, sizeof(t), snap->timing.read_to_publish_avg_us);
    M_PRINTF("\"read_to_publish_avg\":%s,", t);

    fmt_us_to_ms_1dp(t, sizeof(t), snap->timing.read_to_publish_p95_us);
    M_PRINTF("\"read_to_publish_p95\":%s,", t);

    fmt_us_to_ms_1dp(t, sizeof(t), snap->timing.read_to_publish_max_us);
    M_PRINTF("\"read_to_publish_max\":%s,", t);

    fmt_us_to_ms_1dp(t, sizeof(t), snap->timing.pmbus_txn_avg_us);
    M_PRINTF("\"pmbus_txn_avg\":%s,", t);

    fmt_us_to_ms_1dp(t, sizeof(t), snap->timing.pmbus_txn_max_us);
    M_PRINTF("\"pmbus_txn_max\":%s,", t);

    fmt_us_to_ms_1dp(t, sizeof(t), snap->timing.mqtt_publish_avg_us);
    M_PRINTF("\"mqtt_publish_avg\":%s,", t);

    fmt_us_to_ms_1dp(t, sizeof(t), snap->timing.mqtt_publish_max_us);
    M_PRINTF("\"mqtt_publish_max\":%s}", t);

    /* rates (stored as ×10, output with 1 decimal) */
    M_PRINTF(",\"rates\":{"
             "\"telemetry_msgs_per_s\":%u.%u,"
             "\"pmbus_cmds_per_s\":%u.%u}}",
             (unsigned)(snap->rates.telemetry_msgs_per_s_x10 / 10u),
             (unsigned)(snap->rates.telemetry_msgs_per_s_x10 % 10u),
             (unsigned)(snap->rates.pmbus_cmds_per_s_x10 / 10u),
             (unsigned)(snap->rates.pmbus_cmds_per_s_x10 % 10u));

    #undef M_PRINTF

    return (int)(pos - out);
}

/*******************************************************************************
 * Topic builder
 ******************************************************************************/
int build_metrics_topic(char *out, size_t out_sz)
{
    if (out == NULL || out_sz < 8u)
    {
        return -1;
    }

    int len = snprintf(out, out_sz, "%s/metrics", g_config.mqtt.base_topic);

    if (len < 0 || (size_t)len >= out_sz)
    {
        return -1;
    }

    return len;
}
