/*******************************************************************************
 * File Name:   profile_default.h
 *
 * Description: Default configuration profile for the PMBus-MQTT gateway.
 *              Used for normal development and as a baseline for experiments.
 *
 *              2 targets @ 200 ms polling, PEC enabled, RAM buffer only (MVP).
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
        .poll_period_ms  = 2000,
        .status_period_ms = 10000,
    },
    // {
        // .addr_7bit       = 0x59,
        // .label           = "psu_b",
        // .poll_period_ms  = 2000,
        // .status_period_ms = 10000,
    // },
};

/*******************************************************************************
 * Profile definition
 ******************************************************************************/
#define PROFILE_NAME   "default"

#define PROFILE_CONFIG ((config_t){                                             \
    .gw_id = "gw01",                                                           \
                                                                               \
    .i2c = {                                                                   \
        .bus            = 0,                                                   \
        .speed_hz       = 100000,       /* 100 kHz standard SMBus */           \
        .timeout_ms     = 20,                                                  \
        .retries        = 2,                                                   \
        .bus_recovery   = false,                                                \
        .pec_enabled    = true,                                                \
    },                                                                         \
                                                                               \
    .mqtt = {                                                                  \
        .host           = "192.168.1.2",                                      \
        .port           = 1883,                                                \
        .client_id      = "pmbus-gw01",                                        \
        .base_topic     = "pmbus/gw01",                                        \
        .qos            = 1,                                                   \
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
        .persist_seq      = false,      /* MVP: no flash persistence */        \
    },                                                                         \
                                                                               \
    .devices         = k_devices,                                              \
    .num_devices     = sizeof(k_devices) / sizeof(k_devices[0]),               \
    .metrics_period_ms = 2000,                                                 \
})
