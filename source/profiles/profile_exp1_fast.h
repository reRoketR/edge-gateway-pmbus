/*******************************************************************************
 * File Name:   profile_exp1_fast.h
 *
 * Description: Experiment 1 — End-to-end latency measurement.
 *              Fast polling (100 ms) to stress the read→publish pipeline.
 *              Compare with profile_default (200 ms) for latency analysis.
 *
 *              Changes vs default:
 *                - poll_period_ms:  200 → 100
 *                - status_period_ms: 1000 → 2000  (reduce noise)
 *                - metrics_period_ms: 2000 → 1000 (finer granularity)
 *
 * Related Document: agent.md §9 — Exp1
 *
 ******************************************************************************/

#pragma once

#include "gateway_config.h"

static const device_cfg_t k_devices[] = {
    {
        .addr_7bit       = 0x58,
        .label           = "psu_a",
        .poll_period_ms  = 100,
        .status_period_ms = 2000,
    },
    {
        .addr_7bit       = 0x59,
        .label           = "psu_b",
        .poll_period_ms  = 100,
        .status_period_ms = 2000,
    },
};

#define PROFILE_NAME   "exp1_fast"

#define PROFILE_CONFIG ((config_t){                                             \
    .gw_id = "gw01",                                                           \
                                                                               \
    .i2c = {                                                                   \
        .bus            = 0,                                                   \
        .speed_hz       = 100000,                                              \
        .timeout_ms     = 20,                                                  \
        .retries        = 2,                                                   \
        .bus_recovery   = true,                                                \
        .pec_enabled    = true,                                                \
    },                                                                         \
                                                                               \
    .mqtt = {                                                                  \
        .host           = "192.168.1.2",                                      \
        .port           = 1883,                                                \
        .client_id      = "pmbus-gw01",                                        \
        .base_topic     = "pmbus/gw01",                                        \
        .qos_data       = 1,                                                   \
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
        .flush_interval_ms = 100,                                              \
        .drop_oldest      = true,                                              \
        .persist_seq      = false,                                             \
    },                                                                         \
                                                                               \
    .devices         = k_devices,                                              \
    .num_devices     = sizeof(k_devices) / sizeof(k_devices[0]),               \
    .metrics_period_ms = 1000,                                                 \
})
