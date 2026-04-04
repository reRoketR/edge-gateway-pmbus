/*******************************************************************************
 * File Name:   buffer_flush.h
 *
 * Description: Extracted buffer flush logic with injectable publish callback.
 *              Used by mqtt_gw_task.c (production) and integration tests.
 ******************************************************************************/
#ifndef BUFFER_FLUSH_H
#define BUFFER_FLUSH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef bool (*buffer_publish_fn_t)(const char *topic, const char *payload,
                                    size_t payload_len);

/**
 * Flush buffered records (flash first, then RAM).
 * Stops on first publish failure. Respects g_config.buffer.flush_batch_size.
 *
 * @param publish_fn  Publish callback — returns true on success.
 * @return Number of records successfully flushed.
 */
uint16_t buffer_flush_records(buffer_publish_fn_t publish_fn);

#endif /* BUFFER_FLUSH_H */
