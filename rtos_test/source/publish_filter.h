/*******************************************************************************
 * File Name:   publish_filter.h
 *
 * Description: Producer-side publish filtering — telemetry deadband + heartbeat,
 *              status on-change + heartbeat.  Decides whether a record should
 *              be admitted to the pipeline (queue / buffer) or suppressed.
 *
 *              All functions are pure (no I/O, no side-effects beyond updating
 *              the supplied state struct) so they can be unit-tested on the host.
 *
 * Related Document: agent.md §6
 *
 ******************************************************************************/

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "telemetry.h"
#include "gateway_config.h"

/*******************************************************************************
 * Filter state — embedded inside per-device state in pmbus_poll_task.c
 ******************************************************************************/

/** Telemetry filter baseline state. */
typedef struct {
    bool        have_last;          /**< true once first sample emitted       */
    TickType_t  last_emit_tick;
    uint8_t     last_valid_mask;
    int32_t     last_vin_mV;
    uint32_t    last_vout_mV;
    int32_t     last_iin_mA;
    int32_t     last_iout_mA;
    int32_t     last_temp1_mC;
    int32_t     last_pout_mW;
} telem_filter_state_t;

/** Status filter baseline state. */
typedef struct {
    bool        have_last;          /**< true once first sample emitted       */
    TickType_t  last_emit_tick;
    uint8_t     last_valid_mask;
    uint16_t    last_status_word;
    uint8_t     last_status_vout;
    uint8_t     last_status_iout;
    uint8_t     last_status_temp;
} status_filter_state_t;

/*******************************************************************************
 * Filter decisions — these do NOT modify state; caller updates baseline.
 ******************************************************************************/

/**
 * @brief Decide whether a telemetry record should be published.
 *
 * @param rec       Current telemetry record (all fields already filled).
 * @param dev       Per-device config (may contain per-device overrides).
 * @param reporting Global reporting config section from g_config.
 * @param fs        Per-device filter state (read-only; not modified).
 * @param now       Current tick count.
 * @return true if record should be emitted; false to suppress.
 */
bool pf_should_emit_telemetry(const telemetry_record_t *rec,
                              const device_cfg_t *dev,
                              const config_t *cfg,
                              const telem_filter_state_t *fs,
                              TickType_t now);

/**
 * @brief Decide whether a status record should be published.
 */
bool pf_should_emit_status(const status_record_t *rec,
                           const device_cfg_t *dev,
                           const config_t *cfg,
                           const status_filter_state_t *fs,
                           TickType_t now);

/*******************************************************************************
 * Baseline advancement — called ONLY when record is admitted to the pipeline.
 ******************************************************************************/

void pf_advance_telem_baseline(const telemetry_record_t *rec,
                               telem_filter_state_t *fs,
                               TickType_t now);

void pf_advance_status_baseline(const status_record_t *rec,
                                status_filter_state_t *fs,
                                TickType_t now);

