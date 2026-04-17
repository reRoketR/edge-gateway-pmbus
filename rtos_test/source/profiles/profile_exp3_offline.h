/*******************************************************************************
 * File Name:   profile_exp3_offline.h
 *
 * Description: Experiment 3 — Offline buffering test.
 *              Moderate 500 ms poll rate with RAM buffer.  The broker is
 *              stopped mid-run to simulate an outage; the gateway should
 *              buffer records and flush them after reconnection.
 *
 *              Changes vs default:
 *                - poll_period_ms:   500 (unchanged; 2 msgs/s with 1 target)
 *                - status_period_ms: 10000 → 5000
 *                - ram_max_records:  256 (unchanged — fills in ~128 s)
 *                - flash_max_records: 0 → 61 (enable Em_EEPROM persistence)
 *                - metrics_period_ms: 10000 → 2000 (finer monitoring)
 *                - devices:            2 → 1
 *
 * Related Document: agent.md §9 — Exp3, docs/experiments/exp3_offline_buffer.md
 *
 ******************************************************************************/

#pragma once

#include "gateway_config.h"

static const device_cfg_t k_devices[] = {
    {
        .addr_7bit       = 0x58,
        .label           = "psu_a",
        .poll_period_ms  = 500,
        .status_period_ms = 5000,
    },
};

#define PROFILE_NAME   "exp3_offline"

#define PROFILE_CONFIG ((config_t){                                             \
    .gw_id = "gw01",                                                           \
                                                                               \
    .i2c = {                                                                   \
        .bus            = 0,                                                   \
        .speed_hz       = 100000,                                              \
        .transaction_timeout_ms = 20,                                          \
        .retries        = 2,                                                   \
        .bus_recovery   = true,                                                \
        .pec_enabled    = true,                                                \
        .recovery_settle_ms = 5,                                               \
    },                                                                         \
                                                                               \
    .mqtt = {                                                                  \
        .host           = "192.168.1.2",                                       \
        .port           = 1883,                                                \
        .client_id      = "pmbus-gw01",                                        \
        .base_topic     = "pmbus/gw01",                                        \
        .qos_telemetry  = 0,                                                   \
        .qos_control    = 1,                                                   \
        .qos_metrics    = 0,                                                   \
        .backoff_min_ms = 500,                                                 \
        .backoff_max_ms = 10000,                                               \
    },                                                                         \
                                                                               \
    .buffer = {                                                                \
        .enabled          = true,                                              \
        .ram_max_records  = 256,                                               \
        .flash_max_records = 61,        /* Enable Em_EEPROM persistence */     \
        .flush_batch_size = 50,                                                \
        .drop_oldest      = true,                                              \
    },                                                                         \
                                                                               \
    .reporting = {                                                             \
        .telemetry_filter_enabled = true,                                       \
        .status_filter_enabled    = true,                                       \
        .status_emit_initial      = true,                                       \
        .telemetry_heartbeat_ms   = 10000,                                      \
        .status_heartbeat_ms      = 300000,                                     \
        .deadband_vin_mV          = 100,                                        \
        .deadband_vout_mV         = 20,                                         \
        .deadband_iin_mA          = 100,                                        \
        .deadband_iout_mA         = 100,                                        \
        .deadband_temp1_mC        = 1000,                                       \
        .deadband_pout_mW         = 1000,                                       \
    },                                                                         \
                                                                               \
    .devices         = k_devices,                                              \
    .num_devices     = sizeof(k_devices) / sizeof(k_devices[0]),               \
    .metrics_period_ms = 2000,                                                 \
})
