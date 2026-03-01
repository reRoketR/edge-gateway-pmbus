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
 *   - Thread-safe put/get (uses FreeRTOS critical sections)
 *   - drop_oldest policy from g_config.buffer.drop_oldest
 *   - Depth query for metrics gauges
 *   - Flush helper for draining to MQTT
 *   - Flash tier: persistent storage in Em_EEPROM (32 KB, 63 records)
 *     activated when g_config.buffer.flash_max_records > 0
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

/*******************************************************************************
 * Put / Get / Depth
 ******************************************************************************/

/**
 * @brief Enqueue a record into the buffer.
 *
 * Records go to the RAM ring buffer first.  If RAM is full and flash
 * buffering is enabled (flash_max_records > 0), the record spills to
 * flash persistent storage.
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

/*******************************************************************************
 * Buffer task (Task C)
 ******************************************************************************/

/**
 * @brief FreeRTOS housekeeping task for the offline buffer.
 *
 * Periodically updates `buffer_depth_ram` / `buffer_depth_flash` gauge
 * metrics.  Does NOT call cy_mqtt_publish() — all publish operations are
 * serialised through mqtt_gw_task (see flush_buffered_records()).
 *
 * @param[in] pvParameters  Unused
 */
void buffer_task(void *pvParameters);

/** Task stack size (increased for flash I/O + printf in flash_buffer.c) */
#define BUFFER_TASK_STACK_SIZE   (1024u)
#define BUFFER_TASK_PRIORITY     (2u)

/** @} */  /* end of buffer_mgr */
