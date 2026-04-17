/**
 * @file buffer_mgr.h
 * @brief Store-and-forward buffer manager (Task C).
 * @ingroup buffer_mgr
 *
 * @details
 * Two-tier buffering: RAM ring buffer (fast) + optional flash-backed
 * persistent buffer (survives reboot).  Each buffered record stores a
 * pre-encoded JSON string plus its MQTT topic, so that flushing does not
 * require re-encoding.
 *
 * Features:
 *   - Thread-safe put/get with explicit spill/flush serialization
 *   - drop_oldest policy from g_config.buffer.drop_oldest
 *   - Depth query for metrics gauges
 *   - Flush helper for draining to MQTT
 *   - Optional persistent tier selected via `persistent_buffer.h`
 *     (Em_EEPROM by default, QSPI when `BUFFER_BACKEND=QSPI`)
 *
 * @see agent.md §6 (Task C), §8, docs/persistent_buffer.md
 *
 * @defgroup buffer_mgr Buffer Manager
 * @brief Offline store-and-forward buffer: RAM ring + optional flash persistence.
 * @{
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "FreeRTOS.h"
#include "task.h"

/*******************************************************************************
 * Buffered record format
 ******************************************************************************/

/** Max JSON payload size per buffered record */
#define BUFFER_PAYLOAD_MAX  512u

/** Max topic string length */
#define BUFFER_TOPIC_MAX    80u

typedef struct {
    char     topic[BUFFER_TOPIC_MAX];       /**< MQTT topic string           */
    char     payload[BUFFER_PAYLOAD_MAX];   /**< Pre-encoded JSON payload    */
    uint16_t payload_len;                   /**< Actual payload length       */
    uint32_t origin_read_start_ms;          /**< PMBus read-start monotonic ms;
                                                 0 when not telemetry-backed  */
    uint32_t origin_boot_gen;               /**< Boot generation that created
                                                 origin_read_start_ms         */
} buffer_record_t;

/*******************************************************************************
 * Initialization
 ******************************************************************************/

/**
 * @brief Initialise the buffer manager.
 *
 * Allocates the ring buffer from FreeRTOS heap (pvPortMalloc).
 * Must be called once before any put/get operations.
 *
 * @return true on success, false on allocation failure.
 */
bool buffer_mgr_init(void);

/**
 * @brief Initialise the persistent flash tier after the scheduler starts.
 *
 * Required for backends whose erase/write paths rely on RTOS primitives
 * (for example the QSPI serial-flash middleware mutex).
 *
 * Safe to call multiple times; initialization runs at most once.
 */
void buffer_mgr_late_init(void);

/**
 * @brief Register the task that should be notified when buffered data becomes
 *        available for flushing.
 *
 * Passing NULL disables notifications.
 */
void buffer_mgr_register_flush_task(TaskHandle_t handle);

/**
 * @brief Register the task that should be notified when upstream producers
 *        have work ready for spilling into the buffer manager.
 *
 * Passing NULL disables notifications.
 */
void buffer_mgr_register_spill_task(TaskHandle_t handle);

/**
 * @brief Wake the spill task after a successful producer-side enqueue/rescue.
 *
 * Safe to call when the spill task has not registered yet; in that case the
 * signal is ignored.
 */
void buffer_mgr_signal_spill_task(void);

/*******************************************************************************
 * Put / Get / Depth
 ******************************************************************************/

/**
 * @brief Enqueue a record into the buffer.
 *
 * Records go to the RAM ring buffer first. When RAM is full and persistent
 * buffering is enabled, the oldest RAM record is migrated to the persistent
 * tier and the new record stays in RAM. This keeps the persistent tier older
 * than the RAM tier so `persistent -> RAM` flush order remains globally FIFO.
 *
 * If both tiers are full:
 *   - drop_oldest = true  → oldest RAM record is overwritten
 *   - drop_oldest = false → new record is dropped
 *
 * Increments buffer_enqueued and (if applicable) buffer_dropped metrics.
 *
 * @param[in] topic        MQTT topic string
 * @param[in] payload      JSON payload string
 * @param[in] payload_len  Payload length
 *
 * @return true if the record was accepted, false if dropped.
 */
bool buffer_mgr_put(const char *topic, const char *payload, uint16_t payload_len);

/**
 * @brief Get the current boot generation used for buffered latency tracking.
 *
 * This value is compared against `buffer_record_t.origin_boot_gen` so that
 * read-to-publish latency is only recorded for records created in the same
 * boot session.
 */
uint32_t buffer_mgr_current_boot_gen(void);

/**
 * @brief Get the current number of records in the buffer.
 *
 * @return Number of buffered records.
 */
uint32_t buffer_mgr_depth(void);

/*******************************************************************************
 * Peek / Consume (FIFO-safe for flush)
 ******************************************************************************/

/**
 * @brief Peek at the oldest RAM record without removing it.
 *
 * @param[out] out  Pointer to record to fill
 * @return true if a record was copied, false if buffer is empty.
 */
bool buffer_mgr_peek(buffer_record_t *out);

/**
 * @brief Consume (remove) the oldest RAM record after a successful publish.
 *
 * Must be called only after a successful buffer_mgr_peek().
 *
 * @return true if a record was consumed, false if buffer is empty.
 */
bool buffer_mgr_consume(void);

/**
 * @brief Compute read-to-publish latency for a buffered record if it belongs
 *        to the current boot session.
 *
 * @param[in]  rec               Buffered record metadata
 * @param[in]  current_boot_gen  Current boot generation
 * @param[in]  now_ms            Current monotonic time in milliseconds
 * @param[out] out_latency_us    Computed latency in microseconds
 *
 * @return true if the record contributes a valid same-boot sample, false
 *         otherwise.
 */
static inline bool buffer_record_same_boot_latency_us(
    const buffer_record_t *rec,
    uint32_t current_boot_gen,
    uint32_t now_ms,
    uint32_t *out_latency_us)
{
    if (rec == NULL || out_latency_us == NULL)
    {
        return false;
    }

    if (rec->origin_read_start_ms == 0u || rec->origin_boot_gen == 0u)
    {
        return false;
    }

    if (rec->origin_boot_gen != current_boot_gen)
    {
        return false;
    }

    if (now_ms < rec->origin_read_start_ms)
    {
        return false;
    }

    *out_latency_us = (now_ms - rec->origin_read_start_ms) * 1000u;
    return true;
}

/*******************************************************************************
 * Buffer task (Task C)
 ******************************************************************************/

/**
 * @brief FreeRTOS spill task for the offline buffer.
 *
 * Drains upstream queues and rescue rings into buffer_mgr, updates
 * `buffer_depth_ram` / `buffer_depth_flash` gauges, and wakes the MQTT task
 * when new buffered data becomes available.
 * metrics.  Does NOT call cy_mqtt_publish() — all publish operations are
 * serialised through mqtt_gw_task (see flush_buffered_records()).
 *
 * @param[in] pvParameters  Unused
 */
void buffer_task(void *pvParameters);

/** Task stack size (increased for flash I/O + printf in flash_buffer.c) */
#define BUFFER_TASK_STACK_SIZE   (1536u)
#define BUFFER_TASK_PRIORITY     (2u)

#ifdef BUFFER_MGR_HOST_TEST
void buffer_mgr_drain_once(void);
#endif

/** @} */  /* end of buffer_mgr */
