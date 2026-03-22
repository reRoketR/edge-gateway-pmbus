/*******************************************************************************
 * File Name:   pmbus_poll_task.c
 *
 * Description: PMBus polling task implementation (Task A per agent.md §6).
 *
 *              Architecture:
 *                - Maintains per-device timers for telemetry and status polling
 *                - Reads PMBus commands using pmbus_master.h (PDL-based)
 *                - Decodes raw values using pmbus_decode.h (Linear11/16)
 *                - Pushes records to IPC queues
 *                - Updates metrics counters per command outcome
 *                - Emits online/offline events on device state transitions
 *
 *              Timing:
 *                - Uses vTaskDelayUntil for deterministic poll intervals
 *                - The task wakes every POLL_TICK_MS (10 ms) and checks each
 *                  device's deadline independently
 *
 *              Hard rule (agent.md §6): no blocking I2C while holding a mutex
 *              needed by the MQTT task. This task does not hold any mutex.
 *
 * Related Document: agent.md §6 (Task A), §3, §12
 *
 ******************************************************************************/

#include "pmbus_poll_task.h"
#include "pmbus_master.h"
#include "pmbus_decode.h"
#include "telemetry.h"
#include "events.h"
#include "metrics.h"
#include "gateway_config.h"
#include "gateway_ipc.h"
#include "wallclock.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>
#include <string.h>

/*******************************************************************************
 * Constants
 ******************************************************************************/

/** Base tick interval: the task wakes this often to check deadlines. */
#define POLL_TICK_MS    10u

/** Maximum number of devices we support (avoids VLA) */
#define MAX_DEVICES     4u

/** After this many consecutive failures, slow the poll period for the device */
#define OFFLINE_FAIL_THRESHOLD    3u

/** Offline backoff multiplier: poll period × this when device is offline */
#define OFFLINE_BACKOFF_MULT      3u

/** Max VOUT_MODE retry interval (ticks). Retry every ~5 seconds, not every poll */
#define VOUT_MODE_RETRY_TICKS     pdMS_TO_TICKS(5000u)

/** Rate-limit queue-full warnings: print at most once per this many ticks */
#define WARN_THROTTLE_TICKS       pdMS_TO_TICKS(5000u)

/** Enable per-device telemetry cycle start logging for I2C fault diagnosis */
#ifndef GW_DEBUG_LOG_POLL_CYCLE
#define GW_DEBUG_LOG_POLL_CYCLE   1
#endif

/*******************************************************************************
 * Per-device state
 ******************************************************************************/
typedef struct {
    TickType_t  next_telem_tick;     /**< Next telemetry poll deadline (ticks) */
    TickType_t  next_status_tick;    /**< Next status poll deadline (ticks)    */
    TickType_t  next_vout_retry;     /**< Next VOUT_MODE retry deadline        */
    TickType_t  last_telem_warn;     /**< Last telemetry queue-full warning    */
    TickType_t  last_status_warn;    /**< Last status queue-full warning       */
    uint16_t    consec_fails;        /**< Consecutive poll failures            */
    bool        online;             /**< Device considered online?            */
    int8_t      vout_exponent;      /**< Cached VOUT_MODE exponent            */
    bool        vout_exp_valid;     /**< true once VOUT_MODE has been read    */
} device_state_t;

static device_state_t s_dev_state[MAX_DEVICES];

/*******************************************************************************
 * Forward declarations
 ******************************************************************************/
static void poll_telemetry(const device_cfg_t *dev, device_state_t *state);
static void poll_status(const device_cfg_t *dev, device_state_t *state);
static void read_vout_mode(const device_cfg_t *dev, device_state_t *state);
static void check_online_transition(const device_cfg_t *dev,
                                    device_state_t *state, bool success);
static void log_poll_cycle_start(const device_cfg_t *dev,
                                 const device_state_t *state,
                                 uint32_t telemetry_depth);
static bool should_abort_device_cycle(pmbus_status_t st);
static bool defer_polls_for_bus_backoff(device_state_t *state,
                                        TickType_t current);

/*******************************************************************************
 * Task entry point
 ******************************************************************************/
void pmbus_poll_task(void *pvParameters)
{
    (void)pvParameters;

    printf("[POLL] PMBus poll task started\n");

    /* --- Initialise PMBus master driver --- */
    pmbus_status_t init_st = pmbus_init();
    if (init_st != PMBUS_OK)
    {
        printf("[POLL] ERROR: pmbus_init failed (%d). Task will not poll.\n",
               (int)init_st);
        /* Stay alive but do nothing */
        for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
    }

    /* --- Initialise per-device state --- */
    uint8_t num_dev = g_config.num_devices;
    if (num_dev > MAX_DEVICES) num_dev = MAX_DEVICES;

    TickType_t now = xTaskGetTickCount();

    for (uint8_t i = 0; i < num_dev; i++)
    {
        s_dev_state[i].next_telem_tick  = now;
        s_dev_state[i].next_status_tick = now;
        s_dev_state[i].next_vout_retry  = now;
        s_dev_state[i].last_telem_warn  = 0u;
        s_dev_state[i].last_status_warn = 0u;
        s_dev_state[i].consec_fails     = 0u;
        s_dev_state[i].online           = false;
        s_dev_state[i].vout_exp_valid   = false;
        s_dev_state[i].vout_exponent    = -12;  /* sensible default */
    }

    printf("[POLL] Polling %u device(s), tick=%u ms\n",
           (unsigned)num_dev, POLL_TICK_MS);

    /* --- Main loop --- */
    TickType_t wake_tick = now;

    for (;;)
    {
        vTaskDelayUntil(&wake_tick, pdMS_TO_TICKS(POLL_TICK_MS));

        TickType_t current = xTaskGetTickCount();

        for (uint8_t i = 0; i < num_dev; i++)
        {
            const device_cfg_t *dev   = &g_config.devices[i];
            device_state_t     *state = &s_dev_state[i];

            /* Compute effective poll period: if device is offline, back off */
            uint32_t telem_period = dev->poll_period_ms;
            uint32_t status_period = dev->status_period_ms;
            if (state->consec_fails >= OFFLINE_FAIL_THRESHOLD)
            {
                telem_period  *= OFFLINE_BACKOFF_MULT;
                status_period *= OFFLINE_BACKOFF_MULT;
            }

            if (defer_polls_for_bus_backoff(state, current))
            {
                continue;
            }

            /* --- Telemetry poll --- */
            if ((int32_t)(current - state->next_telem_tick) >= 0)
            {
                uint32_t telemetry_depth = gateway_ipc_telemetry_queue_depth();
                metrics_set_telemetry_queue_depth(telemetry_depth);
                log_poll_cycle_start(dev, state, telemetry_depth);
                poll_telemetry(dev, state);
                state->next_telem_tick = current +
                    pdMS_TO_TICKS(telem_period);
            }

            if (defer_polls_for_bus_backoff(state, current))
            {
                continue;
            }

            /* --- Status poll --- */
            if ((int32_t)(current - state->next_status_tick) >= 0)
            {
                poll_status(dev, state);
                state->next_status_tick = current +
                    pdMS_TO_TICKS(status_period);
            }
        }
    }
}

/*******************************************************************************
 * Telemetry polling — one full read cycle per device
 ******************************************************************************/

static bool defer_polls_for_bus_backoff(device_state_t *state,
                                        TickType_t current)
{
    uint32_t remaining_ms = 0u;
    TickType_t defer_ticks;
    TickType_t defer_until;

    if (!pmbus_bus_backoff_active(&remaining_ms))
    {
        return false;
    }

    defer_ticks = pdMS_TO_TICKS(remaining_ms);
    if ((defer_ticks == 0u) && (remaining_ms != 0u))
    {
        defer_ticks = 1u;
    }

    defer_until = current + defer_ticks;
    if ((int32_t)(state->next_telem_tick - defer_until) < 0)
    {
        state->next_telem_tick = defer_until;
    }
    if ((int32_t)(state->next_status_tick - defer_until) < 0)
    {
        state->next_status_tick = defer_until;
    }

    return true;
}

/** Helper: read a single command byte, update counters, return true on success.
 *  Used for PMBus Read Byte commands: STATUS_VOUT (0x7A), STATUS_IOUT (0x7B),
 *  STATUS_TEMPERATURE (0x7D), etc. */
static bool read_byte_cmd(uint8_t addr, uint8_t cmd, uint8_t *out,
                          uint8_t *retries_total,
                          pmbus_status_t *out_status)
{
    uint8_t retries_used = 0;
    pmbus_status_t st = pmbus_read_byte(addr, cmd, out, &retries_used);
    if (out_status != NULL)
    {
        *out_status = st;
    }

    /* Always account for actual retries consumed inside the driver */
    if (retries_total != NULL)
    {
        *retries_total += retries_used;
    }
    for (uint8_t ri = 0; ri < retries_used; ri++)
    {
        metrics_inc_pmbus_retries();
    }

    if (st == PMBUS_OK)
    {
        metrics_inc_pmbus_reads_ok();
        return true;
    }

    metrics_inc_pmbus_reads_fail();
    switch (st)
    {
        case PMBUS_ERR_TIMEOUT:  metrics_inc_pmbus_timeouts(); break;
        case PMBUS_ERR_NACK:     metrics_inc_pmbus_nack();     break;
        case PMBUS_ERR_PEC:      metrics_inc_pmbus_pec_fail(); break;
        default: break;
    }
    return false;
}

/** Helper: read a single command word, update counters, return true on success */
static bool read_cmd(uint8_t addr, uint8_t cmd, uint16_t *out,
                     uint8_t *retries_total,
                     pmbus_status_t *out_status)
{
    uint8_t retries_used = 0;
    pmbus_status_t st = pmbus_read_word(addr, cmd, out, &retries_used);
    if (out_status != NULL)
    {
        *out_status = st;
    }

    /* Always account for actual retries consumed inside the driver */
    if (retries_total != NULL)
    {
        *retries_total += retries_used;
    }
    for (uint8_t ri = 0; ri < retries_used; ri++)
    {
        metrics_inc_pmbus_retries();
    }

    if (st == PMBUS_OK)
    {
        metrics_inc_pmbus_reads_ok();
        return true;
    }

    /* Classify the error for metrics */
    metrics_inc_pmbus_reads_fail();
    switch (st)
    {
        case PMBUS_ERR_TIMEOUT:  metrics_inc_pmbus_timeouts(); break;
        case PMBUS_ERR_NACK:     metrics_inc_pmbus_nack();     break;
        case PMBUS_ERR_PEC:      metrics_inc_pmbus_pec_fail(); break;
        default: break;
    }
    return false;
}

static bool should_abort_device_cycle(pmbus_status_t st)
{
    switch (st)
    {
        case PMBUS_ERR_TIMEOUT:
        case PMBUS_ERR_NACK:
        case PMBUS_ERR_ARB_LOST:
        case PMBUS_ERR_BUS_FAULT:
        case PMBUS_ERR_NOT_READY:
        case PMBUS_ERR_RECOVERY_FAIL:
        case PMBUS_ERR_NOT_INIT:
        case PMBUS_ERR_INIT:
            return true;
        case PMBUS_OK:
        case PMBUS_ERR_PEC:
        case PMBUS_ERR_ARG:
        default:
            return false;
    }
}

/*******************************************************************************
 * Pretty-print one telemetry record as an ASCII table row.
 * Header is printed once every TELEM_TABLE_HEADER_ROWS rows.
 *
 * Gated behind GW_DEBUG_LOG_TELEM so experiment/production profiles can
 * suppress the high-bandwidth UART output.  Define GW_DEBUG_LOG_TELEM=1
 * in the Makefile (DEFINES+=) or in a profile header to enable.
 ******************************************************************************/
#ifndef GW_DEBUG_LOG_TELEM
#define GW_DEBUG_LOG_TELEM  0
#endif

#if GW_DEBUG_LOG_TELEM
#define TELEM_TABLE_HEADER_ROWS  20u

static void log_telemetry_table(const telemetry_record_t *r)
{
    static uint16_t s_row = 0u;

    if (s_row % TELEM_TABLE_HEADER_ROWS == 0u)
    {
        printf("\n[POLL] %-6s  %8s  %8s  %8s  %8s  %8s  %8s  %5s  %5s\n",
               "ADDR", "VIN(V)", "VOUT(V)", "IIN(A)", "IOUT(A)",
               "TEMP(C)", "POUT(W)", "ms", "rty");
        printf("[POLL] ------  --------  --------  --------  "
               "--------  --------  --------  -----  -----\n");
    }
    s_row++;

    /* Convert milli-units to float for display; show '--' when invalid */
    char vin_s[10], vout_s[10], iin_s[10], iout_s[10], temp_s[10], pout_s[10];

#define FMT_FIELD(buf, mask_bit, val_milli) \
    if (r->valid_mask & (mask_bit)) \
        snprintf((buf), sizeof(buf), "%8.3f", (float)(val_milli) / 1000.0f); \
    else \
        snprintf((buf), sizeof(buf), "%8s", "--")

    FMT_FIELD(vin_s,  TELEM_VALID_VIN,   r->vin_mV);
    FMT_FIELD(vout_s, TELEM_VALID_VOUT,  r->vout_mV);
    FMT_FIELD(iin_s,  TELEM_VALID_IIN,   r->iin_mA);
    FMT_FIELD(iout_s, TELEM_VALID_IOUT,  r->iout_mA);
    FMT_FIELD(temp_s, TELEM_VALID_TEMP1, r->temp1_mC);
    FMT_FIELD(pout_s, TELEM_VALID_POUT,  r->pout_mW);

#undef FMT_FIELD

    printf("[POLL] 0x%02X   %s  %s  %s  %s  %s  %s  %5u  %5u\n",
           r->addr_7bit,
           vin_s, vout_s, iin_s, iout_s, temp_s, pout_s,
           (unsigned)r->read_ms, (unsigned)r->retries);
}
#else
static inline void log_telemetry_table(const telemetry_record_t *r)
{
    (void)r;  /* Telemetry table logging disabled */
}
#endif /* GW_DEBUG_LOG_TELEM */

#if GW_DEBUG_LOG_POLL_CYCLE
static void log_poll_cycle_start(const device_cfg_t *dev,
                                 const device_state_t *state,
                                 uint32_t telemetry_depth)
{
    printf("[POLL] cycle start addr=0x%02X label=\"%s\" online=%d fails=%u "
           "qdepth=%lu mqtt=%d t=%lu\n",
           (unsigned)dev->addr_7bit,
           dev->label,
           (int)state->online,
           (unsigned)state->consec_fails,
           (unsigned long)telemetry_depth,
           (int)gateway_ipc_is_mqtt_online(),
           (unsigned long)gateway_ipc_monotonic_ms());
}
#else
static inline void log_poll_cycle_start(const device_cfg_t *dev,
                                        const device_state_t *state,
                                        uint32_t telemetry_depth)
{
    (void)dev;
    (void)state;
    (void)telemetry_depth;
}
#endif /* GW_DEBUG_LOG_POLL_CYCLE */

static void poll_telemetry(const device_cfg_t *dev, device_state_t *state)
{
    uint8_t addr = dev->addr_7bit;
    telemetry_record_t rec;
    memset(&rec, 0, sizeof(rec));

    /* Cache VOUT_MODE exponent if not yet read — rate-limited retries */
    if (!state->vout_exp_valid)
    {
        TickType_t now_t = xTaskGetTickCount();
        if ((int32_t)(now_t - state->next_vout_retry) >= 0)
        {
            read_vout_mode(dev, state);
            state->next_vout_retry = now_t + VOUT_MODE_RETRY_TICKS;
        }
    }

    TickType_t t_start = xTaskGetTickCount();
    rec.read_start_ms = gateway_ipc_monotonic_ms();
    uint8_t total_retries = 0u;
    uint16_t raw;
    bool any_ok = false;
    pmbus_status_t last_st = PMBUS_OK;

    /* READ_VIN (Linear11) */
    if (read_cmd(addr, PMBUS_CMD_READ_VIN, &raw, &total_retries, &last_st))
    {
        rec.raw_vin    = raw;
        rec.vin_mV     = pmbus_linear11_to_milli(raw);
        rec.valid_mask |= TELEM_VALID_VIN;
        any_ok = true;
    }
    else if (should_abort_device_cycle(last_st))
    {
        goto telemetry_done;
    }

    /* READ_VOUT (Linear16) */
    if (read_cmd(addr, PMBUS_CMD_READ_VOUT, &raw, &total_retries, &last_st))
    {
        rec.raw_vout   = raw;
        rec.vout_mV    = pmbus_linear16_to_mv(raw, state->vout_exponent);
        rec.valid_mask |= TELEM_VALID_VOUT;
        any_ok = true;
    }
    else if (should_abort_device_cycle(last_st))
    {
        goto telemetry_done;
    }

    /* READ_IIN (Linear11) */
    if (read_cmd(addr, PMBUS_CMD_READ_IIN, &raw, &total_retries, &last_st))
    {
        rec.raw_iin    = raw;
        rec.iin_mA     = pmbus_linear11_to_milli(raw);
        rec.valid_mask |= TELEM_VALID_IIN;
        any_ok = true;
    }
    else if (should_abort_device_cycle(last_st))
    {
        goto telemetry_done;
    }

    /* READ_IOUT (Linear11) */
    if (read_cmd(addr, PMBUS_CMD_READ_IOUT, &raw, &total_retries, &last_st))
    {
        rec.raw_iout   = raw;
        rec.iout_mA    = pmbus_linear11_to_milli(raw);
        rec.valid_mask |= TELEM_VALID_IOUT;
        any_ok = true;
    }
    else if (should_abort_device_cycle(last_st))
    {
        goto telemetry_done;
    }

    /* READ_TEMPERATURE_1 (Linear11) */
    if (read_cmd(addr, PMBUS_CMD_READ_TEMPERATURE_1, &raw, &total_retries,
                 &last_st))
    {
        rec.raw_temp1  = raw;
        rec.temp1_mC   = pmbus_linear11_to_milli(raw);
        rec.valid_mask |= TELEM_VALID_TEMP1;
        any_ok = true;
    }
    else if (should_abort_device_cycle(last_st))
    {
        goto telemetry_done;
    }

    /* READ_POUT (Linear11) */
    if (read_cmd(addr, PMBUS_CMD_READ_POUT, &raw, &total_retries, &last_st))
    {
        rec.raw_pout   = raw;
        rec.pout_mW    = pmbus_linear11_to_milli(raw);
        rec.valid_mask |= TELEM_VALID_POUT;
        any_ok = true;
    }

telemetry_done:
    TickType_t t_end = xTaskGetTickCount();
    uint32_t read_ms = (uint32_t)(t_end - t_start) * portTICK_PERIOD_MS;

    /* Record PMBus transaction latency for metrics */
    metrics_record_pmbus_txn_us(read_ms * 1000u);

    /* Fill metadata */
    rec.ts_ms    = gateway_ipc_now_ms();
    rec.time_synced = wallclock_is_synced();
    rec.seq      = gateway_ipc_next_seq();
    rec.addr_7bit = addr;
    rec.label    = dev->label;
    rec.pec      = g_config.i2c.pec_enabled;
    rec.read_ms  = (uint16_t)read_ms;
    rec.retries  = total_retries;

    /* Update consecutive-fail counter BEFORE online/offline transition check */
    if (any_ok)
    {
        state->consec_fails = 0u;
    }
    else
    {
        if (state->consec_fails < UINT16_MAX)
            state->consec_fails++;
    }

    /* Track online/offline transition (uses updated consec_fails) */
    check_online_transition(dev, state, any_ok);

    if (!any_ok)
    {
        /* Don't queue empty records — waste of queue space */
        return;
    }

    log_telemetry_table(&rec);

    /* Push to telemetry queue (non-blocking — drop if full) */
    if (xQueueSend(gateway_ipc_telemetry_queue(), &rec, 0) == pdTRUE)
    {
        metrics_inc_telemetry_enqueued();
    }
    else
    {
        TickType_t now_t = xTaskGetTickCount();
        if ((int32_t)(now_t - state->last_telem_warn) >= (int32_t)WARN_THROTTLE_TICKS)
        {
            printf("[POLL] WARN: telemetry queue full (addr=0x%02X)\n", addr);
            state->last_telem_warn = now_t;
            gateway_ipc_post_event(EVT_QUEUE_OVERFLOW, "telemetry_queue");
        }
        metrics_inc_queue_drops();
    }
}

/*******************************************************************************
 * Status polling
 ******************************************************************************/
static void poll_status(const device_cfg_t *dev, device_state_t *state)
{
    uint8_t addr = dev->addr_7bit;
    status_record_t rec;
    memset(&rec, 0, sizeof(rec));

    uint16_t raw;
    pmbus_status_t last_st = PMBUS_OK;

    /* STATUS_WORD (16-bit — Read Word) */
    if (read_cmd(addr, PMBUS_CMD_STATUS_WORD, &raw, NULL, &last_st))
    {
        rec.status_word = raw;
        rec.valid_mask |= STATUS_VALID_WORD;
    }
    else if (should_abort_device_cycle(last_st))
    {
        goto status_done;
    }

    /* STATUS_VOUT (8-bit — Read Byte) */
    uint8_t raw8;
    if (read_byte_cmd(addr, PMBUS_CMD_STATUS_VOUT, &raw8, NULL, &last_st))
    {
        rec.status_vout = raw8;
        rec.valid_mask |= STATUS_VALID_VOUT;
    }
    else if (should_abort_device_cycle(last_st))
    {
        goto status_done;
    }

    /* STATUS_IOUT (8-bit — Read Byte) */
    if (read_byte_cmd(addr, PMBUS_CMD_STATUS_IOUT, &raw8, NULL, &last_st))
    {
        rec.status_iout = raw8;
        rec.valid_mask |= STATUS_VALID_IOUT;
    }
    else if (should_abort_device_cycle(last_st))
    {
        goto status_done;
    }

    /* STATUS_TEMPERATURE (8-bit — Read Byte) */
    if (read_byte_cmd(addr, PMBUS_CMD_STATUS_TEMPERATURE, &raw8, NULL,
                      &last_st))
    {
        rec.status_temperature = raw8;
        rec.valid_mask |= STATUS_VALID_TEMP;
    }

status_done:
    /* Don't queue empty status records when device is offline */
    if (rec.valid_mask == 0u)
    {
        return;
    }

    /* Fill metadata */
    rec.ts_ms     = gateway_ipc_now_ms();
    rec.time_synced = wallclock_is_synced();
    rec.seq       = gateway_ipc_next_seq();
    rec.addr_7bit = addr;
    rec.label     = dev->label;

    /* Push to status queue */
    if (xQueueSend(gateway_ipc_status_queue(), &rec, 0) != pdTRUE)
    {
        TickType_t now_t = xTaskGetTickCount();
        if ((int32_t)(now_t - state->last_status_warn) >= (int32_t)WARN_THROTTLE_TICKS)
        {
            printf("[POLL] WARN: status queue full (addr=0x%02X)\n", addr);
            state->last_status_warn = now_t;
        }
    }
}

/*******************************************************************************
 * VOUT_MODE read (cached per device)
 ******************************************************************************/
static void read_vout_mode(const device_cfg_t *dev, device_state_t *state)
{
    uint8_t raw;
    pmbus_status_t st = pmbus_read_byte(dev->addr_7bit,
                                        PMBUS_CMD_VOUT_MODE, &raw, NULL);
    if (st == PMBUS_OK)
    {
        state->vout_exponent = pmbus_vout_mode_exponent(raw);
        state->vout_exp_valid = true;
        printf("[POLL] 0x%02X VOUT_MODE exp=%d\n",
               dev->addr_7bit, (int)state->vout_exponent);
    }
    else
    {
        /* Use default exponent; will retry after VOUT_MODE_RETRY_TICKS */
        static uint8_t s_vout_warn_count = 0u;
        if (s_vout_warn_count < 4u)
        {
            printf("[POLL] 0x%02X VOUT_MODE read failed, using exp=%d (retry in 5s)\n",
                   dev->addr_7bit, (int)state->vout_exponent);
            s_vout_warn_count++;
        }
    }
}

/*******************************************************************************
 * Online/offline transition tracking
 ******************************************************************************/
static void check_online_transition(const device_cfg_t *dev,
                                    device_state_t *state, bool success)
{
    if (success && !state->online)
    {
        /* Device came online (1 successful poll is enough) */
        state->online = true;
        char detail[EVT_DETAIL_MAX];
        snprintf(detail, sizeof(detail), "addr=0x%02X", dev->addr_7bit);
        gateway_ipc_post_event(EVT_PMBUS_DEVICE_ONLINE, detail);
        printf("[POLL] Device 0x%02X \"%s\" ONLINE\n",
               dev->addr_7bit, dev->label);
    }
    else if (!success && state->online &&
             state->consec_fails >= OFFLINE_FAIL_THRESHOLD)
    {
        /* Device went offline after N consecutive failures (avoids flicker) */
        state->online = false;
        char detail[EVT_DETAIL_MAX];
        snprintf(detail, sizeof(detail), "addr=0x%02X,consec_fails=%u",
                 dev->addr_7bit, (unsigned)state->consec_fails);
        gateway_ipc_post_event(EVT_PMBUS_DEVICE_OFFLINE, detail);
        printf("[POLL] Device 0x%02X \"%s\" OFFLINE (after %u fails)\n",
               dev->addr_7bit, dev->label, (unsigned)state->consec_fails);
    }
}

/* [] END OF FILE */
