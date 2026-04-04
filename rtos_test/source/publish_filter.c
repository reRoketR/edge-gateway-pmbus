/*******************************************************************************
 * File Name:   publish_filter.c
 *
 * Description: Implementation of producer-side publish filtering.
 *              All functions are pure (no I/O, no globals) and can be tested
 *              on the host without FreeRTOS or PMBus stubs.
 *
 * Related Document: agent.md §6
 *
 ******************************************************************************/

#include "publish_filter.h"

/*******************************************************************************
 * Internal helpers
 ******************************************************************************/

/** Resolve per-device override: if device value is 0, use global default. */
static inline uint32_t resolve_u32(uint32_t per_device, uint32_t global)
{
    return (per_device != 0u) ? per_device : global;
}

/** Absolute difference exceeds deadband (signed fields). */
static inline bool exceeds_deadband_s(int32_t a, int32_t b, uint32_t db)
{
    int32_t diff = a - b;
    return ((diff >= 0) ? (uint32_t)diff : (uint32_t)(-diff)) >= db;
}

/** Absolute difference exceeds deadband (unsigned fields, e.g. vout_mV). */
static inline bool exceeds_deadband_u(uint32_t a, uint32_t b, uint32_t db)
{
    uint32_t diff = (a >= b) ? (a - b) : (b - a);
    return diff >= db;
}

/*******************************************************************************
 * Telemetry filter
 ******************************************************************************/

bool pf_should_emit_telemetry(const telemetry_record_t *rec,
                              const device_cfg_t *dev,
                              const config_t *cfg,
                              const telem_filter_state_t *fs,
                              TickType_t now)
{
    if (!cfg->reporting.telemetry_filter_enabled)
        return true;

    /* First sample or offline→online transition */
    if (!fs->have_last)
        return true;

    /* valid_mask changed */
    if (rec->valid_mask != fs->last_valid_mask)
        return true;

    /* Heartbeat: time since last emit */
    uint32_t hb = resolve_u32(dev->telemetry_heartbeat_ms,
                              cfg->reporting.telemetry_heartbeat_ms);
    if ((int32_t)(now - fs->last_emit_tick) >= (int32_t)pdMS_TO_TICKS(hb))
        return true;

    /* Deadband: check each valid field (OR across fields) */
    const uint8_t vm = rec->valid_mask & fs->last_valid_mask;

    if ((vm & TELEM_VALID_VIN) &&
        exceeds_deadband_s(rec->vin_mV, fs->last_vin_mV,
                           resolve_u32(dev->deadband_vin_mV,
                                       cfg->reporting.deadband_vin_mV)))
        return true;

    if ((vm & TELEM_VALID_VOUT) &&
        exceeds_deadband_u(rec->vout_mV, fs->last_vout_mV,
                           resolve_u32(dev->deadband_vout_mV,
                                       cfg->reporting.deadband_vout_mV)))
        return true;

    if ((vm & TELEM_VALID_IIN) &&
        exceeds_deadband_s(rec->iin_mA, fs->last_iin_mA,
                           resolve_u32(dev->deadband_iin_mA,
                                       cfg->reporting.deadband_iin_mA)))
        return true;

    if ((vm & TELEM_VALID_IOUT) &&
        exceeds_deadband_s(rec->iout_mA, fs->last_iout_mA,
                           resolve_u32(dev->deadband_iout_mA,
                                       cfg->reporting.deadband_iout_mA)))
        return true;

    if ((vm & TELEM_VALID_TEMP1) &&
        exceeds_deadband_s(rec->temp1_mC, fs->last_temp1_mC,
                           resolve_u32(dev->deadband_temp1_mC,
                                       cfg->reporting.deadband_temp1_mC)))
        return true;

    if ((vm & TELEM_VALID_POUT) &&
        exceeds_deadband_s(rec->pout_mW, fs->last_pout_mW,
                           resolve_u32(dev->deadband_pout_mW,
                                       cfg->reporting.deadband_pout_mW)))
        return true;

    return false;
}

/*******************************************************************************
 * Status filter
 ******************************************************************************/

bool pf_should_emit_status(const status_record_t *rec,
                           const device_cfg_t *dev,
                           const config_t *cfg,
                           const status_filter_state_t *fs,
                           TickType_t now)
{
    if (!cfg->reporting.status_filter_enabled)
        return true;

    /* First sample or offline→online transition */
    if (!fs->have_last)
        return cfg->reporting.status_emit_initial;

    /* valid_mask changed */
    if (rec->valid_mask != fs->last_valid_mask)
        return true;

    /* Any status register changed (exact compare, no deadband) */
    if (rec->status_word != fs->last_status_word ||
        rec->status_vout != fs->last_status_vout ||
        rec->status_iout != fs->last_status_iout ||
        rec->status_temperature != fs->last_status_temp)
        return true;

    /* Optional heartbeat (0 = disabled) */
    uint32_t hb = resolve_u32(dev->status_heartbeat_ms,
                              cfg->reporting.status_heartbeat_ms);
    if (hb > 0u &&
        (int32_t)(now - fs->last_emit_tick) >= (int32_t)pdMS_TO_TICKS(hb))
        return true;

    return false;
}

/*******************************************************************************
 * Baseline advancement
 ******************************************************************************/

void pf_advance_telem_baseline(const telemetry_record_t *rec,
                               telem_filter_state_t *fs,
                               TickType_t now)
{
    fs->have_last       = true;
    fs->last_emit_tick  = now;
    fs->last_valid_mask = rec->valid_mask;
    fs->last_vin_mV     = rec->vin_mV;
    fs->last_vout_mV    = rec->vout_mV;
    fs->last_iin_mA     = rec->iin_mA;
    fs->last_iout_mA    = rec->iout_mA;
    fs->last_temp1_mC   = rec->temp1_mC;
    fs->last_pout_mW    = rec->pout_mW;
}

void pf_advance_status_baseline(const status_record_t *rec,
                                status_filter_state_t *fs,
                                TickType_t now)
{
    fs->have_last       = true;
    fs->last_emit_tick  = now;
    fs->last_valid_mask = rec->valid_mask;
    fs->last_status_word = rec->status_word;
    fs->last_status_vout = rec->status_vout;
    fs->last_status_iout = rec->status_iout;
    fs->last_status_temp = rec->status_temperature;
}
