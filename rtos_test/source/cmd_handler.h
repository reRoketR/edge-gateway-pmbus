/**
 * @file cmd_handler.h
 * @brief Remote SMBus command handler — types, parse, encode, dedupe.
 * @ingroup cmd_handler
 *
 * @details
 * Provides the data structures and helpers for the MQTT → I²C command path:
 *
 *   1. MQTT callback copies raw payload into cmd_raw_t
 *   2. MQTT task parses cmd_raw_t → cmd_request_t via cmd_handler_parse()
 *   3. pmbus_poll_task executes cmd_request_t → cmd_response_t
 *   4. MQTT task encodes cmd_response_t → JSON via cmd_handler_encode_response()
 *
 * Also manages:
 *   - Recent-response cache (depth CMD_CACHE_DEPTH) for QoS 1 dedup
 *   - In-flight ID tracker (depth CMD_INFLIGHT_DEPTH)
 *
 * @see agent.md §14 (safety note)
 *
 * @defgroup cmd_handler Remote Command Handler
 * @brief MQTT → I²C remote command parse / encode / dedupe.
 * @{
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "pmbus_master.h"  /* pmbus_status_t */

/*******************************************************************************
 * Constants
 ******************************************************************************/

/** Max correlation ID length including NUL terminator */
#define CMD_ID_MAX              16u

/** Max write payload bytes */
#define CMD_MAX_WRITE_LEN       32u

/** Max read payload bytes */
#define CMD_MAX_READ_LEN        32u

/** Max raw MQTT JSON payload bytes (including NUL) */
#define CMD_RAW_PAYLOAD_MAX     256u

/** Queue depths for cmd_raw, cmd_request, cmd_response */
#define CMD_QUEUE_DEPTH         4u

/** Recent-response cache depth */
#define CMD_CACHE_DEPTH         4u

/** In-flight ID tracker depth.
 *  Must cover worst-case pipeline: CMD_QUEUE_DEPTH requests +
 *  CMD_QUEUE_DEPTH responses + 1 pending slot = 2*CMD_QUEUE_DEPTH + 1. */
#define CMD_INFLIGHT_DEPTH      (2u * CMD_QUEUE_DEPTH + 1u)

/*******************************************************************************
 * Command-layer status codes (non-I²C errors)
 ******************************************************************************/

/** Command-layer status codes, used alongside pmbus_status_t values. */
typedef enum {
    CMD_STATUS_OK           = 0,    /**< I²C transaction succeeded          */
    CMD_STATUS_BAD_JSON     = 100,  /**< Malformed JSON payload             */
    CMD_STATUS_BAD_REQUEST  = 101,  /**< Valid JSON but invalid field values */
    CMD_STATUS_UNSUPPORTED  = 102,  /**< Unsupported transfer shape          */
    CMD_STATUS_QUEUE_FULL   = 103,  /**< cmd_request_queue is full          */
} cmd_status_t;

/*******************************************************************************
 * IPC data types
 ******************************************************************************/

/**
 * @brief Raw command payload from MQTT callback.
 *
 * Copied verbatim from the MQTT receive callback into cmd_raw_queue.
 * Parsed later in the MQTT task main loop context.
 */
typedef struct {
    char     payload[CMD_RAW_PAYLOAD_MAX]; /**< NUL-terminated JSON string  */
    uint16_t payload_len;                  /**< Actual payload length       */
} cmd_raw_t;

/**
 * @brief Parsed command request: MQTT task → pmbus_poll_task.
 *
 * All fields are validated before enqueue.
 */
typedef struct {
    char    id[CMD_ID_MAX];                /**< Correlation ID (NUL-term)   */
    uint8_t addr_7bit;                     /**< 7-bit I²C address           */
    uint8_t write_data[CMD_MAX_WRITE_LEN]; /**< Write payload bytes         */
    uint8_t write_len;                     /**< Number of write bytes       */
    uint8_t read_len;                      /**< Number of bytes to read     */
    bool    pec;                           /**< PEC enabled for this xfer   */
} cmd_request_t;

/**
 * @brief Command response: pmbus_poll_task → MQTT task.
 */
typedef struct {
    char    id[CMD_ID_MAX];                /**< Echoed correlation ID       */
    uint8_t addr_7bit;                     /**< Echoed I²C address          */
    uint8_t status;                        /**< pmbus_status_t or cmd_status_t */
    uint8_t read_data[CMD_MAX_READ_LEN];   /**< Read data (valid on OK)     */
    uint8_t read_len;                      /**< Actual bytes read           */
    uint16_t exec_ms;                      /**< I²C transaction duration ms */
} cmd_response_t;

/*******************************************************************************
 * Parse result
 ******************************************************************************/

/** Result of cmd_handler_parse(). */
typedef enum {
    CMD_PARSE_OK,               /**< Request parsed and validated          */
    CMD_PARSE_BAD_JSON,         /**< JSON malformed, id NOT recovered      */
    CMD_PARSE_BAD_JSON_WITH_ID, /**< JSON malformed, but id was recovered  */
    CMD_PARSE_BAD_REQUEST,      /**< JSON valid, but fields out of range   */
    CMD_PARSE_UNSUPPORTED,      /**< Unsupported transfer shape            */
} cmd_parse_result_t;

/*******************************************************************************
 * Initialization
 ******************************************************************************/

/**
 * @brief Initialise the command handler state (cache, in-flight tracker).
 *
 * Must be called once before any parse/encode/cache operations.
 */
void cmd_handler_init(void);

/*******************************************************************************
 * Parse / Encode
 ******************************************************************************/

/**
 * @brief Parse a raw JSON command payload into a cmd_request_t.
 *
 * @param[in]  raw     Raw payload from cmd_raw_queue
 * @param[out] req     Parsed request (populated on CMD_PARSE_OK)
 * @param[out] id_out  Recovered ID string (populated when possible, even on
 *                     parse failure). NUL-terminated, max CMD_ID_MAX bytes.
 *
 * @return Parse result code.
 */
cmd_parse_result_t cmd_handler_parse(const cmd_raw_t *raw,
                                     cmd_request_t *req,
                                     char id_out[CMD_ID_MAX]);

/**
 * @brief Encode a cmd_response_t into a JSON string.
 *
 * Output format:
 *   {"id":"...","addr":N,"status":"...","data":[N,...],"exec_ms":N}
 *
 * @param[in]  resp    Response to encode
 * @param[out] buf     Output buffer
 * @param[in]  buf_sz  Output buffer size
 *
 * @return Number of characters written (excluding NUL), or -1 on truncation.
 */
int cmd_handler_encode_response(const cmd_response_t *resp,
                                char *buf, size_t buf_sz);

/**
 * @brief Build an error response for a command-layer failure.
 *
 * Populates a cmd_response_t with the given status and no data.
 *
 * @param[out] resp    Response to populate
 * @param[in]  id      Correlation ID
 * @param[in]  addr    I²C address (0 if unknown)
 * @param[in]  status  cmd_status_t value
 */
void cmd_handler_build_error(cmd_response_t *resp,
                             const char *id, uint8_t addr,
                             cmd_status_t status);

/*******************************************************************************
 * Status string mapping
 ******************************************************************************/

/**
 * @brief Map a status code (pmbus_status_t or cmd_status_t) to a string.
 *
 * @param[in] status  Status code
 * @return NUL-terminated string (e.g. "OK", "NACK", "BAD_JSON").
 */
const char *cmd_status_str(uint8_t status);

/*******************************************************************************
 * Recent-response cache (QoS 1 dedup)
 ******************************************************************************/

/**
 * @brief Look up a recently published response by ID.
 *
 * @param[in]  id    Correlation ID to search for
 * @param[out] resp  Cached response (copied out if found)
 *
 * @return true if cache hit, false if not found.
 */
bool cmd_cache_lookup(const char *id, cmd_response_t *resp);

/**
 * @brief Store a published response in the cache.
 *
 * Overwrites the oldest entry if cache is full (ring buffer).
 *
 * @param[in] resp  Response to cache
 */
void cmd_cache_put(const cmd_response_t *resp);

/*******************************************************************************
 * In-flight ID tracker
 ******************************************************************************/

/**
 * @brief Check if a command ID is currently in-flight.
 *
 * @param[in] id  Correlation ID to check
 * @return true if the ID is in-flight, false otherwise.
 */
bool cmd_inflight_check(const char *id);

/**
 * @brief Mark a command ID as in-flight.
 *
 * @param[in] id  Correlation ID to mark
 * @return true if added, false if tracker is full (caller should reject).
 */
bool cmd_inflight_add(const char *id);

/**
 * @brief Remove a command ID from the in-flight tracker.
 *
 * @param[in] id  Correlation ID to remove
 */
void cmd_inflight_remove(const char *id);

/** @} */  /* end of cmd_handler */
