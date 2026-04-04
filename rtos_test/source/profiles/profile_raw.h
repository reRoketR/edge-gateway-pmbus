/*******************************************************************************
 * File Name:   profile_raw.h
 *
 * Description: Raw-data profile — all publish filtering disabled.
 *              Every telemetry and status read is enqueued unconditionally.
 *              Useful for baseline measurements and debugging.
 *
 *              Changes vs default:
 *                - telemetry_filter_enabled: true → false
 *                - status_filter_enabled:    true → false
 *
 * Related Document: agent.md §5
 *
 ******************************************************************************/

#pragma once

#include "gateway_config.h"

static const device_cfg_t k_devices[] = {
    {
        .addr_7bit       = 0x58,
        .label           = "psu_a",
        .poll_period_ms  = 500,
        .status_period_ms = 10000,
    },
    {
        .addr_7bit       = 0x59,
        .label           = "psu_b",
        .poll_period_ms  = 500,
        .status_period_ms = 10000,
    },
};

#define PROFILE_NAME   "raw"

#define PROFILE_CONFIG ((config_t){                                             \
    .gw_id = "thesis_gw01",                                                    \
                                                                               \
    .i2c = {                                                                   \
        .bus            = 0,                                                   \
        .speed_hz       = 100000,                                              \
        .transaction_timeout_ms = 20,                                          \
        .retries        = 2,                                                   \
        .bus_recovery   = false,                                               \
        .pec_enabled    = true,                                                \
        .recovery_settle_ms = 5,                                               \
    },                                                                         \
                                                                               \
    .mqtt = {                                                                  \
        .host           = "192.168.1.6",                                       \
        .port           = 1883,                                                \
        .client_id      = "pmbus-thesis-gw01",                                 \
        .base_topic     = "pmbus/thesis_gw01",                                 \
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
        .flash_max_records = 2048,                                             \
        .flush_batch_size = 50,                                                \
        .flush_interval_ms = 200,                                              \
        .drop_oldest      = true,                                              \
    },                                                                         \
                                                                               \
    .reporting = {                                                             \
        .telemetry_filter_enabled = false,  /* <<< No filtering */             \
        .status_filter_enabled    = false,  /* <<< No filtering */             \
        .status_emit_initial      = true,                                      \
        .telemetry_heartbeat_ms   = 10000,                                     \
        .status_heartbeat_ms      = 300000,                                    \
        .deadband_vin_mV          = 100,                                       \
        .deadband_vout_mV         = 20,                                        \
        .deadband_iin_mA          = 100,                                       \
        .deadband_iout_mA         = 100,                                       \
        .deadband_temp1_mC        = 1000,                                      \
        .deadband_pout_mW         = 1000,                                      \
    },                                                                         \
                                                                               \
    .devices         = k_devices,                                              \
    .num_devices     = sizeof(k_devices) / sizeof(k_devices[0]),               \
    .metrics_period_ms = 10000,                                                \
})
