/*******************************************************************************
 * File Name:   profile_exp2_throughput.h
 *
 * Description: Experiment 2 — Throughput & stability stress test.
 *              Aggressive 50 ms poll to find the maximum sustainable
 *              telemetry throughput before queue overflow or error growth.
 *
 *              Changes vs default:
 *                - poll_period_ms:   2000 → 50  (20 msgs/s target)
 *                - status_period_ms: 10000 → 5000
 *                - metrics_period_ms: 2000 → 1000 (fine granularity)
 *                - 1 device only (isolate publish bottleneck)
 *
 * Related Document: agent.md §9 — Exp2, docs/experiments/exp2_throughput.md
 *
 ******************************************************************************/

#pragma once

#include "gateway_config.h"

static const device_cfg_t k_devices[] = {
    {
        .addr_7bit       = 0x58,
        .label           = "psu_a",
        .poll_period_ms  = 50,
        .status_period_ms = 5000,
    },
};

#define PROFILE_NAME   "exp2_throughput"

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
        .host           = "192.168.1.2",                                       \
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
        .flush_interval_ms = 50,       /* Fast flush for throughput test */     \
        .drop_oldest      = true,                                              \
        .persist_seq      = false,                                             \
    },                                                                         \
                                                                               \
    .devices         = k_devices,                                              \
    .num_devices     = sizeof(k_devices) / sizeof(k_devices[0]),               \
    .metrics_period_ms = 1000,                                                 \
})
