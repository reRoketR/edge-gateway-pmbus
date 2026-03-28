/*******************************************************************************
 * File Name:   profile_default.h
 *
 * Description: Default configuration profile for the PMBus-MQTT gateway.
 *              Used for normal development and as a baseline for experiments.
 *
 *              2 targets @ 500 ms polling, PEC enabled, RAM buffer only.
 *
 * Related Document: agent.md §5
 *
 ******************************************************************************/

#pragma once

#include "gateway_config.h"

/*******************************************************************************
 * Device table
 ******************************************************************************/
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

/*******************************************************************************
 * Profile definition
 ******************************************************************************/
#define PROFILE_NAME   "default"

#define PROFILE_CONFIG ((config_t){                                             \
    .gw_id = "thesis_gw01",                                                           \
                                                                               \
    .i2c = {                                                                   \
        .bus            = 0,                                                   \
        .speed_hz       = 100000,       /* 100 kHz standard SMBus */           \
        .transaction_timeout_ms = 20,                                          \
        .retries        = 2,                                                   \
        .bus_recovery   = false,                                               \
        .pec_enabled    = true,                                                \
        .recovery_settle_ms = 5,        /* D1-3: settle after recovery */      \
    },                                                                         \
                                                                               \
    .mqtt = {                                                                  \
        .host           = "broker.hivemq.com",                                      \
        .port           = 1883,                                                \
        .client_id      = "pmbus-thesis-gw01",                                        \
        .base_topic     = "pmbus/thesis_gw01",                                        \
        .qos_telemetry  = 0,            /* latency-sensitive stream */          \
        .qos_control    = 1,            /* status/events must be reliable */    \
        .qos_metrics    = 0,            /* metrics: fire-and-forget */          \
        .backoff_min_ms = 500,                                                 \
        .backoff_max_ms = 10000,                                               \
    },                                                                         \
                                                                               \
    .buffer = {                                                                \
        .enabled          = true,                                              \
        .ram_max_records  = 256,                                               \
        .flash_max_records = 0,         /* MVP: RAM-only */                    \
        .flush_batch_size = 50,                                                \
        .flush_interval_ms = 200,                                              \
        .drop_oldest      = true,                                              \
    },                                                                         \
                                                                               \
    .devices         = k_devices,                                              \
    .num_devices     = sizeof(k_devices) / sizeof(k_devices[0]),               \
    .metrics_period_ms = 10000,         /* 10 s — diagnostics, not real-time */\
})
