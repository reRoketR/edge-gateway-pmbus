/**
 * @file buffer_mgr.h
 * @brief Store-and-forward buffer manager (Task C).
 * @ingroup buffer_mgr
 *
 * @details
 * MVP implementation: RAM-only ring buffer.  Each buffered record stores a
 * pre-encoded JSON string plus its MQTT topic, so that flushing does not
 * require re-encoding.
 *
 * Features:
 *   - Thread-safe put/get (uses FreeRTOS critical sections)
 *   - drop_oldest policy from g_config.buffer.drop_oldest
 *   - Depth query for metrics gauges
 *   - Flush helper for draining to MQTT
 *
 * @see agent.md §6 (Task C), §8
 *
 * @defgroup buffer_mgr Buffer Manager
 * @brief Offline store-and-forward RAM ring buffer with drop-oldest policy.
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
 * If the buffer is full:
 *   - drop_oldest = true  → oldest record is overwritten
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
 * @brief Dequeue the oldest record from the buffer.
 *
 * @param[out] out  Pointer to record to fill
 *
 * @return true if a record was dequeued, false if buffer is empty.
 */
bool buffer_mgr_get(buffer_record_t *out);

/**
 * @brief Get the current number of records in the buffer.
 *
 * @return Number of buffered records.
 */
uint32_t buffer_mgr_depth(void);

/*******************************************************************************
 * Buffer task (Task C)
 ******************************************************************************/

/**
 * @brief FreeRTOS task that flushes buffered records to MQTT.
 *
 * When MQTT is online, dequeues up to flush_batch_size records per tick
 * and publishes them. Runs at low-medium priority.
 *
 * @param[in] pvParameters  Unused
 */
void buffer_task(void *pvParameters);

/** Task stack & priority */
#define BUFFER_TASK_STACK_SIZE   (512u)
#define BUFFER_TASK_PRIORITY     (2u)

/** @} */  /* end of buffer_mgr */
