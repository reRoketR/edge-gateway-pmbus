/**
 * @file buffer_record.h
 * @brief Compact binary record schema used by the store-and-forward buffer.
 * @ingroup buffer_mgr
 *
 * @details
 * The buffer stores compact binary records in RAM and in persistent flash.
 * JSON is generated only at publish time.
 *
 * Records are intentionally stable across host/target builds:
 *   - only fixed-width integer fields
 *   - no raw pointers in serialized payloads
 *   - topic strings are reconstructed from record kind + device address
 *
 * @see buffer_mgr.h, buffer_flush.c, flash_buffer.h, qspi_buffer.h
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "telemetry.h"
#include "events.h"

/*******************************************************************************
 * Record kinds
 ******************************************************************************/
typedef enum {
    BUFFER_RECORD_TELEMETRY = 0u,
    BUFFER_RECORD_STATUS    = 1u,
    BUFFER_RECORD_EVENT     = 2u,
} buffer_record_kind_t;

/*******************************************************************************
 * Binary payloads
 *
 * These are the on-buffer / on-flash payloads. They intentionally exclude
 * any publish-time derived data such as MQTT topic strings and device labels.
 ******************************************************************************/

typedef struct __attribute__((packed)) {
    uint64_t ts_ms;
    uint32_t seq;
    uint8_t  addr_7bit;
    uint8_t  time_synced;
    uint8_t  pec;
    uint16_t read_ms;
    uint8_t  retries;
    uint8_t  valid_mask;

    int32_t  vin_mV;
    uint32_t vout_mV;
    int32_t  iin_mA;
    int32_t  iout_mA;
    int32_t  temp1_mC;
    int32_t  pout_mW;

    uint16_t raw_vin;
    uint16_t raw_vout;
    uint16_t raw_iin;
    uint16_t raw_iout;
    uint16_t raw_temp1;
    uint16_t raw_pout;
} buffer_telemetry_payload_t;

typedef struct __attribute__((packed)) {
    uint64_t ts_ms;
    uint32_t seq;
    uint8_t  addr_7bit;
    uint8_t  time_synced;

    uint16_t status_word;
    uint8_t  status_vout;
    uint8_t  status_iout;
    uint8_t  status_temperature;
    uint8_t  valid_mask;
} buffer_status_payload_t;

typedef struct __attribute__((packed)) {
    uint64_t ts_ms;
    uint8_t  time_synced;
    uint8_t  type;
    uint8_t  detail_len;
    char     detail[EVT_DETAIL_MAX];
} buffer_event_payload_t;

/*******************************************************************************
 * Generic record wrapper
 ******************************************************************************/
typedef struct {
    uint8_t  kind;                  /**< buffer_record_kind_t            */
    uint8_t  reserved[3];           /**< Alignment / future use          */
    uint32_t origin_read_start_ms;   /**< Telemetry read-start origin      */
    uint32_t origin_boot_gen;       /**< Boot generation for same-boot lat */

    union {
        buffer_telemetry_payload_t telemetry;
        buffer_status_payload_t    status;
        buffer_event_payload_t     event;
    } payload;
} buffer_record_t;

_Static_assert(sizeof(buffer_telemetry_payload_t) < 512u,
               "telemetry payload must remain compact");
_Static_assert(sizeof(buffer_status_payload_t) < 512u,
               "status payload must remain compact");
_Static_assert(sizeof(buffer_event_payload_t) < 512u,
               "event payload must remain compact");

/*******************************************************************************
 * Helpers
 ******************************************************************************/

static inline uint16_t buffer_record_payload_len_for_kind(uint8_t kind)
{
    switch ((buffer_record_kind_t)kind)
    {
        case BUFFER_RECORD_TELEMETRY:
            return (uint16_t)sizeof(buffer_telemetry_payload_t);
        case BUFFER_RECORD_STATUS:
            return (uint16_t)sizeof(buffer_status_payload_t);
        case BUFFER_RECORD_EVENT:
            return (uint16_t)sizeof(buffer_event_payload_t);
        default:
            return 0u;
    }
}

static inline uint16_t buffer_record_payload_len(const buffer_record_t *rec)
{
    if (rec == NULL)
    {
        return 0u;
    }
    return buffer_record_payload_len_for_kind(rec->kind);
}

static inline const void *buffer_record_payload_ptr_const(const buffer_record_t *rec)
{
    if (rec == NULL)
    {
        return NULL;
    }

    switch ((buffer_record_kind_t)rec->kind)
    {
        case BUFFER_RECORD_TELEMETRY: return &rec->payload.telemetry;
        case BUFFER_RECORD_STATUS:    return &rec->payload.status;
        case BUFFER_RECORD_EVENT:     return &rec->payload.event;
        default:                      return NULL;
    }
}

static inline void *buffer_record_payload_ptr(buffer_record_t *rec)
{
    if (rec == NULL)
    {
        return NULL;
    }

    switch ((buffer_record_kind_t)rec->kind)
    {
        case BUFFER_RECORD_TELEMETRY: return &rec->payload.telemetry;
        case BUFFER_RECORD_STATUS:    return &rec->payload.status;
        case BUFFER_RECORD_EVENT:     return &rec->payload.event;
        default:                      return NULL;
    }
}

static inline void buffer_record_clear(buffer_record_t *rec, buffer_record_kind_t kind)
{
    if (rec == NULL)
    {
        return;
    }

    memset(rec, 0, sizeof(*rec));
    rec->kind = (uint8_t)kind;
}

static inline void buffer_record_from_telemetry(buffer_record_t *dst,
                                                const telemetry_record_t *src,
                                                uint32_t origin_boot_gen)
{
    if (dst == NULL || src == NULL)
    {
        return;
    }

    buffer_record_clear(dst, BUFFER_RECORD_TELEMETRY);
    dst->origin_read_start_ms = src->read_start_ms;
    dst->origin_boot_gen = origin_boot_gen;

    dst->payload.telemetry.ts_ms = src->ts_ms;
    dst->payload.telemetry.seq = src->seq;
    dst->payload.telemetry.addr_7bit = src->addr_7bit;
    dst->payload.telemetry.time_synced = src->time_synced ? 1u : 0u;
    dst->payload.telemetry.pec = src->pec ? 1u : 0u;
    dst->payload.telemetry.read_ms = src->read_ms;
    dst->payload.telemetry.retries = src->retries;
    dst->payload.telemetry.valid_mask = src->valid_mask;
    dst->payload.telemetry.vin_mV = src->vin_mV;
    dst->payload.telemetry.vout_mV = src->vout_mV;
    dst->payload.telemetry.iin_mA = src->iin_mA;
    dst->payload.telemetry.iout_mA = src->iout_mA;
    dst->payload.telemetry.temp1_mC = src->temp1_mC;
    dst->payload.telemetry.pout_mW = src->pout_mW;
    dst->payload.telemetry.raw_vin = src->raw_vin;
    dst->payload.telemetry.raw_vout = src->raw_vout;
    dst->payload.telemetry.raw_iin = src->raw_iin;
    dst->payload.telemetry.raw_iout = src->raw_iout;
    dst->payload.telemetry.raw_temp1 = src->raw_temp1;
    dst->payload.telemetry.raw_pout = src->raw_pout;
}

static inline void buffer_record_from_status(buffer_record_t *dst,
                                             const status_record_t *src)
{
    if (dst == NULL || src == NULL)
    {
        return;
    }

    buffer_record_clear(dst, BUFFER_RECORD_STATUS);
    dst->origin_read_start_ms = 0u;
    dst->origin_boot_gen = 0u;

    dst->payload.status.ts_ms = src->ts_ms;
    dst->payload.status.seq = src->seq;
    dst->payload.status.addr_7bit = src->addr_7bit;
    dst->payload.status.time_synced = src->time_synced ? 1u : 0u;
    dst->payload.status.status_word = src->status_word;
    dst->payload.status.status_vout = src->status_vout;
    dst->payload.status.status_iout = src->status_iout;
    dst->payload.status.status_temperature = src->status_temperature;
    dst->payload.status.valid_mask = src->valid_mask;
}

static inline void buffer_record_from_event(buffer_record_t *dst,
                                            const event_record_t *src)
{
    if (dst == NULL || src == NULL)
    {
        return;
    }

    buffer_record_clear(dst, BUFFER_RECORD_EVENT);
    dst->origin_read_start_ms = 0u;
    dst->origin_boot_gen = 0u;

    dst->payload.event.ts_ms = src->ts_ms;
    dst->payload.event.time_synced = src->time_synced ? 1u : 0u;
    dst->payload.event.type = (uint8_t)src->type;

    size_t detail_len = strnlen(src->detail, EVT_DETAIL_MAX - 1u);
    if (detail_len >= EVT_DETAIL_MAX)
    {
        detail_len = EVT_DETAIL_MAX - 1u;
    }
    dst->payload.event.detail_len = (uint8_t)detail_len;
    memcpy(dst->payload.event.detail, src->detail, detail_len);
    dst->payload.event.detail[detail_len] = '\0';
}

static inline void buffer_record_to_telemetry(const buffer_record_t *src,
                                              telemetry_record_t *dst,
                                              const char *label)
{
    if (src == NULL || dst == NULL)
    {
        return;
    }

    memset(dst, 0, sizeof(*dst));
    dst->ts_ms = src->payload.telemetry.ts_ms;
    dst->read_start_ms = src->origin_read_start_ms;
    dst->time_synced = (src->payload.telemetry.time_synced != 0u);
    dst->boot_count = src->origin_boot_gen;
    dst->seq = src->payload.telemetry.seq;
    dst->addr_7bit = src->payload.telemetry.addr_7bit;
    dst->label = label;
    dst->pec = (src->payload.telemetry.pec != 0u);
    dst->read_ms = src->payload.telemetry.read_ms;
    dst->retries = src->payload.telemetry.retries;
    dst->vin_mV = src->payload.telemetry.vin_mV;
    dst->vout_mV = src->payload.telemetry.vout_mV;
    dst->iin_mA = src->payload.telemetry.iin_mA;
    dst->iout_mA = src->payload.telemetry.iout_mA;
    dst->temp1_mC = src->payload.telemetry.temp1_mC;
    dst->pout_mW = src->payload.telemetry.pout_mW;
    dst->raw_vin = src->payload.telemetry.raw_vin;
    dst->raw_vout = src->payload.telemetry.raw_vout;
    dst->raw_iin = src->payload.telemetry.raw_iin;
    dst->raw_iout = src->payload.telemetry.raw_iout;
    dst->raw_temp1 = src->payload.telemetry.raw_temp1;
    dst->raw_pout = src->payload.telemetry.raw_pout;
    dst->valid_mask = src->payload.telemetry.valid_mask;
}

static inline void buffer_record_to_status(const buffer_record_t *src,
                                          status_record_t *dst,
                                          const char *label)
{
    if (src == NULL || dst == NULL)
    {
        return;
    }

    memset(dst, 0, sizeof(*dst));
    dst->ts_ms = src->payload.status.ts_ms;
    dst->time_synced = (src->payload.status.time_synced != 0u);
    dst->seq = src->payload.status.seq;
    dst->addr_7bit = src->payload.status.addr_7bit;
    dst->label = label;
    dst->status_word = src->payload.status.status_word;
    dst->status_vout = src->payload.status.status_vout;
    dst->status_iout = src->payload.status.status_iout;
    dst->status_temperature = src->payload.status.status_temperature;
    dst->valid_mask = src->payload.status.valid_mask;
}

static inline void buffer_record_to_event(const buffer_record_t *src,
                                         event_record_t *dst)
{
    if (src == NULL || dst == NULL)
    {
        return;
    }

    memset(dst, 0, sizeof(*dst));
    dst->ts_ms = src->payload.event.ts_ms;
    dst->time_synced = (src->payload.event.time_synced != 0u);
    dst->type = (event_type_t)src->payload.event.type;

    size_t detail_len = src->payload.event.detail_len;
    if (detail_len >= EVT_DETAIL_MAX)
    {
        detail_len = EVT_DETAIL_MAX - 1u;
    }
    memcpy(dst->detail, src->payload.event.detail, detail_len);
    dst->detail[detail_len] = '\0';
}
