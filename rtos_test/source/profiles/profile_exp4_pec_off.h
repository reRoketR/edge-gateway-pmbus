/*******************************************************************************
 * File Name:   profile_exp4_pec_off.h
 *
 * Description: Experiment 4 — Bus reliability & PEC cost.
 *              PEC disabled to compare error rates and latency impact
 *              against profiles with PEC enabled.
 *
 *              Changes vs default:
 *                - pec_enabled: true → false
 *                - poll_period_ms: 500 → 200
 *                - bus_recovery:  false → true
 *                - metrics_period_ms: 10000 → 1000 (finer granularity)
 *
 * Related Document: agent.md §9 — Exp4
 *
 ******************************************************************************/

#pragma once

#include "gateway_config.h"

static const device_cfg_t k_devices[] = {
    {
        .addr_7bit       = 0x58,
        .label           = "psu_a",
        .poll_period_ms  = 200,
        .status_period_ms = 1000,
    },
    {
        .addr_7bit       = 0x59,
        .label           = "psu_b",
        .poll_period_ms  = 200,
        .status_period_ms = 1000,
    },
};

#define PROFILE_NAME   "exp4_pec_off"

#define PROFILE_CONFIG ((config_t){                                             \
    .gw_id = "gw01",                                                           \
                                                                               \
    .i2c = {                                                                   \
        .bus            = 0,                                                   \
        .speed_hz       = 100000,                                              \
        .transaction_timeout_ms = 20,                                          \
        .retries        = 2,                                                   \
        .bus_recovery   = true,                                                \
        .pec_enabled    = false,        /* <<< PEC disabled for Exp4 */        \
        .recovery_settle_ms = 5,                                               \
    },                                                                         \
                                                                               \
    .mqtt = {                                                                  \
        .host           = "192.168.1.2",                                      \
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
        .flash_max_records = 0,                                                \
        .flush_batch_size = 50,                                                \
        .flush_interval_ms = 200,                                              \
        .drop_oldest      = true,                                              \
    },                                                                         \
                                                                               \
    .devices         = k_devices,                                              \
    .num_devices     = sizeof(k_devices) / sizeof(k_devices[0]),               \
    .metrics_period_ms = 1000,                                                 \
})
