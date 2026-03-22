/*******************************************************************************
 * File Name:   telemetry.c
 *
 * Description: JSON encoding for TelemetryRecord and StatusRecord.
 *              All encoding uses snprintf — no heap, no dynamic allocation.
 *
 *              Milli-unit → SI float conversion:
 *                vin_mV=12030 → "vin":12.03
 *                Uses integer division to produce fixed 2-3 decimal places.
 *
 * Related Document: agent.md §7, docs/mqtt_topics.md §4.1-4.2
 *
 ******************************************************************************/

/* Enable C99-compliant printf on MinGW (for %llu support in host tests) */
#if defined(__MINGW32__) || defined(__MINGW64__)
#define __USE_MINGW_ANSI_STDIO 1
#endif

#include "telemetry.h"
#include "gateway_config.h"
#include "gw_util.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>

/*******************************************************************************
 * Private helpers
 ******************************************************************************/

/**
 * @brief Append a formatted string to a buffer with bounds checking.
 *
 * @param[in,out] pos    Current write position (updated on success)
 * @param[in]     end    Pointer past end of buffer
 * @param[in]     fmt    printf format string
 * @param[in]     ...    Format arguments
 *
 * @return Number of chars written, or -1 if buffer exhausted.
 */
static int buf_printf(char **pos, const char *end, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 3, 4)))
#endif
;

static int buf_printf(char **pos, const char *end, const char *fmt, ...)
{
    if (*pos >= end) return -1;

    va_list args;
    va_start(args, fmt);
    int avail = (int)(end - *pos);
    int written = vsnprintf(*pos, (size_t)avail, fmt, args);
    va_end(args);

    if (written < 0 || written >= avail)
    {
        return -1;  /* Buffer too small */
    }

    *pos += written;
    return written;
}

/* fmt_u64() is now provided by gw_util.h */

/**
 * @brief Format a milli-unit signed value as a float string with 2 decimals.
 *
 * Examples: 12030 → "12.03",  -500 → "-0.50",  0 → "0.00"
 */
static int fmt_milli_2dp(char *buf, size_t sz, int32_t milli)
{
    int32_t integer  = milli / 1000;
    int32_t fraction = milli % 1000;

    /* Handle negative fractions: e.g. -500 → integer=0, fraction=-500 */
    if (milli < 0 && integer == 0)
    {
        if (fraction < 0) fraction = -fraction;
        return snprintf(buf, sz, "-0.%02d", (int)(fraction / 10));
    }

    if (fraction < 0) fraction = -fraction;

    return snprintf(buf, sz, "%d.%02d", (int)integer, (int)(fraction / 10));
}

/**
 * @brief Format an unsigned milli-unit value as a float string with 2 decimals.
 */
static int fmt_milli_u_2dp(char *buf, size_t sz, uint32_t milli)
{
    uint32_t integer  = milli / 1000u;
    uint32_t fraction = milli % 1000u;

    return snprintf(buf, sz, "%u.%02u", (unsigned)integer,
                    (unsigned)(fraction / 10u));
}

/*******************************************************************************
 * Telemetry JSON encoding
 ******************************************************************************/
int encode_telemetry_json(const telemetry_record_t *rec,
                          char *out, size_t out_sz)
{
    if (rec == NULL || out == NULL || out_sz < 64u)
    {
        return -1;
    }

    char *pos = out;
    const char *end = out + out_sz;
    char val_buf[16];
    char ts_buf[24];

    fmt_u64(ts_buf, sizeof(ts_buf), rec->ts_ms);

    /* Open + metadata */
    if (buf_printf(&pos, end,
            "{\"ts_ms\":%s,\"time_synced\":%s,\"seq\":%u,"
            "\"gw_id\":\"%s\",\"addr\":\"0x%02X\",\"label\":\"%s\","
            "\"pec\":%s,\"read_ms\":%u,\"retries\":%u",
            ts_buf,
            rec->time_synced ? "true" : "false",
            (unsigned)rec->seq,
            g_config.gw_id,
            (unsigned)rec->addr_7bit,
            rec->label ? rec->label : "?",
            rec->pec ? "true" : "false",
            (unsigned)rec->read_ms,
            (unsigned)rec->retries) < 0)
    {
        return -1;
    }

    /* Voltage group: "v": {"vin": ..., "vout": ...} */
    {
        bool has_vin  = (rec->valid_mask & TELEM_VALID_VIN)  != 0u;
        bool has_vout = (rec->valid_mask & TELEM_VALID_VOUT) != 0u;

        if (has_vin || has_vout)
        {
            if (buf_printf(&pos, end, ",\"v\":{") < 0) return -1;

            bool first = true;
            if (has_vin)
            {
                fmt_milli_2dp(val_buf, sizeof(val_buf), rec->vin_mV);
                if (buf_printf(&pos, end, "\"vin\":%s", val_buf) < 0) return -1;
                first = false;
            }
            if (has_vout)
            {
                fmt_milli_u_2dp(val_buf, sizeof(val_buf), rec->vout_mV);
                if (buf_printf(&pos, end, "%s\"vout\":%s",
                               first ? "" : ",", val_buf) < 0) return -1;
            }
            if (buf_printf(&pos, end, "}") < 0) return -1;
        }
    }

    /* Current group: "a": {"iin": ..., "iout": ...} */
    {
        bool has_iin  = (rec->valid_mask & TELEM_VALID_IIN)  != 0u;
        bool has_iout = (rec->valid_mask & TELEM_VALID_IOUT) != 0u;

        if (has_iin || has_iout)
        {
            if (buf_printf(&pos, end, ",\"a\":{") < 0) return -1;

            bool first = true;
            if (has_iin)
            {
                fmt_milli_2dp(val_buf, sizeof(val_buf), rec->iin_mA);
                if (buf_printf(&pos, end, "\"iin\":%s", val_buf) < 0) return -1;
                first = false;
            }
            if (has_iout)
            {
                fmt_milli_2dp(val_buf, sizeof(val_buf), rec->iout_mA);
                if (buf_printf(&pos, end, "%s\"iout\":%s",
                               first ? "" : ",", val_buf) < 0) return -1;
            }
            if (buf_printf(&pos, end, "}") < 0) return -1;
        }
    }

    /* Temperature group: "c": {"temp1": ...} */
    if (rec->valid_mask & TELEM_VALID_TEMP1)
    {
        /* temp1 is in milli-°C, display with 1 decimal → /100 for 0.1°C */
        int32_t temp_deci = rec->temp1_mC / 100;  /* deci-°C */
        int32_t t_int = temp_deci / 10;
        int32_t t_frac = temp_deci % 10;
        if (t_frac < 0) t_frac = -t_frac;

        if (rec->temp1_mC < 0 && t_int == 0)
        {
            if (buf_printf(&pos, end, ",\"c\":{\"temp1\":-%d.%d}",
                           (int)t_int, (int)t_frac) < 0) return -1;
        }
        else
        {
            if (buf_printf(&pos, end, ",\"c\":{\"temp1\":%d.%d}",
                           (int)t_int, (int)t_frac) < 0) return -1;
        }
    }

    /* Power group: "w": {"pout": ...} */
    if (rec->valid_mask & TELEM_VALID_POUT)
    {
        fmt_milli_2dp(val_buf, sizeof(val_buf), rec->pout_mW);
        if (buf_printf(&pos, end, ",\"w\":{\"pout\":%s}", val_buf) < 0)
            return -1;
    }

    /* Raw hex group (always include read_vout for reference) */
    if (rec->valid_mask & TELEM_VALID_VOUT)
    {
        if (buf_printf(&pos, end, ",\"raw\":{\"read_vout\":\"0x%04X\"}",
                       (unsigned)rec->raw_vout) < 0)
            return -1;
    }

    /* Close */
    if (buf_printf(&pos, end, "}") < 0) return -1;

    return (int)(pos - out);
}

/*******************************************************************************
 * Status JSON encoding
 ******************************************************************************/
int encode_status_json(const status_record_t *rec,
                       char *out, size_t out_sz)
{
    if (rec == NULL || out == NULL || out_sz < 64u)
    {
        return -1;
    }

    char ts_buf[24];
    fmt_u64(ts_buf, sizeof(ts_buf), rec->ts_ms);

    char *pos = out;
    const char *end = out + out_sz;
    int avail, w;

    #define S_PRINTF(fmt, ...) do {                                    \
        avail = (int)(end - pos);                                      \
        if (avail <= 0) return -1;                                     \
        w = snprintf(pos, (size_t)avail, fmt, ##__VA_ARGS__);          \
        if (w < 0 || w >= avail) return -1;                            \
        pos += w;                                                      \
    } while(0)

    S_PRINTF("{\"ts_ms\":%s,\"time_synced\":%s,\"seq\":%u,"
             "\"gw_id\":\"%s\",\"addr\":\"0x%02X\",\"label\":\"%s\"",
             ts_buf,
             rec->time_synced ? "true" : "false",
             (unsigned)rec->seq,
             g_config.gw_id,
             (unsigned)rec->addr_7bit,
             rec->label ? rec->label : "?");

    /* Only emit fields whose data was actually read successfully */
    if (rec->valid_mask & STATUS_VALID_WORD)
        S_PRINTF(",\"status_word\":\"0x%04X\"", (unsigned)rec->status_word);
    if (rec->valid_mask & STATUS_VALID_VOUT)
        S_PRINTF(",\"status_vout\":\"0x%02X\"", (unsigned)rec->status_vout);
    if (rec->valid_mask & STATUS_VALID_IOUT)
        S_PRINTF(",\"status_iout\":\"0x%02X\"", (unsigned)rec->status_iout);
    if (rec->valid_mask & STATUS_VALID_TEMP)
        S_PRINTF(",\"status_temperature\":\"0x%02X\"", (unsigned)rec->status_temperature);

    S_PRINTF("}");

    #undef S_PRINTF

    return (int)(pos - out);
}

/*******************************************************************************
 * MQTT topic builder
 ******************************************************************************/
int build_device_topic(char *out, size_t out_sz,
                       uint8_t addr_7bit, const char *suffix)
{
    if (out == NULL || suffix == NULL || out_sz < 16u)
    {
        return -1;
    }

    int len = snprintf(out, out_sz, "%s/dev/0x%02X/%s",
                       g_config.mqtt.base_topic,
                       (unsigned)addr_7bit,
                       suffix);

    if (len < 0 || (size_t)len >= out_sz)
    {
        return -1;
    }

    return len;
}
