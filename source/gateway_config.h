/**
 * @file gateway_config.h
 * @brief Compile-time configuration types for the PMBus-MQTT gateway.
 * @ingroup gateway_config
 *
 * @details
 * The active configuration is defined by the selected profile header
 * (profile_default.h, profile_exp1_fast.h, etc.).  All experimental
 * parameters are compile-time constants for thesis repeatability —
 * YAML/JSON parsing is intentionally NOT used on the gateway MCU.
 *
 * @see agent.md §5
 *
 * @defgroup gateway_config Gateway Configuration
 * @brief Compile-time configuration system with experiment profiles.
 * @{
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/*******************************************************************************
 * Device configuration (per PMBus target on the bus)
 ******************************************************************************/
typedef struct {
    uint8_t     addr_7bit;          /**< 7-bit I2C/SMBus address              */
    const char *label;              /**< Human-readable label, e.g. "psu_a"   */
    uint32_t    poll_period_ms;     /**< Telemetry polling interval (ms)      */
    uint32_t    status_period_ms;   /**< Status register polling interval (ms)*/
} device_cfg_t;

/*******************************************************************************
 * Top-level gateway configuration
 ******************************************************************************/
typedef struct {
    const char *gw_id;              /**< Gateway identifier, e.g. "gw01"     */

    /* ---- I2C / SMBus master ---- */
    struct {
        uint8_t  bus;               /**< I2C bus index (0 = CYBSP_I2C)       */
        uint32_t speed_hz;          /**< Bus clock: 100000 or 400000         */
        uint32_t timeout_ms;        /**< Per-transaction timeout             */
        uint8_t  retries;           /**< Max retries per command             */
        bool     bus_recovery;      /**< Attempt clock-stretching recovery   */
        bool     pec_enabled;       /**< SMBus PEC (CRC-8) on every txn     */
    } i2c;

    /* ---- MQTT broker ---- */
    struct {
        const char *host;           /**< Broker hostname / IP                */
        uint16_t    port;           /**< Broker port (1883 / 8883)           */
        const char *client_id;      /**< MQTT client ID                      */
        const char *base_topic;     /**< Topic prefix, e.g. "pmbus/gw01"    */
        uint8_t     qos;            /**< QoS level (0, 1, 2)                */
        uint32_t    backoff_min_ms; /**< Reconnect backoff minimum           */
        uint32_t    backoff_max_ms; /**< Reconnect backoff maximum           */
    } mqtt;

    /* ---- Store-and-forward buffer ---- */
    struct {
        bool     enabled;           /**< Master switch for buffering         */
        uint16_t ram_max_records;   /**< Max records in RAM ring buffer      */
        uint32_t flash_max_records; /**< Max records in flash (0 = disabled) */
        uint16_t flush_batch_size;  /**< Records to flush per tick           */
        uint32_t flush_interval_ms; /**< Flush timer period                  */
        bool     drop_oldest;       /**< true = drop oldest on overflow      */
        bool     persist_seq;       /**< Persist seq counter across reboot   */
    } buffer;

    /* ---- Device list ---- */
    const device_cfg_t *devices;    /**< Array of target devices             */
    uint8_t  num_devices;           /**< Number of entries in devices[]      */

    /* ---- Metrics ---- */
    uint32_t metrics_period_ms;     /**< Metrics publish interval (ms)       */
} config_t;

/*******************************************************************************
 * Global config instance (defined in gateway_config.c)
 ******************************************************************************/
extern const config_t  g_config;
extern const char     *g_profile_name;

/*******************************************************************************
 * Helper: print active configuration to UART for thesis reproducibility.
 * Must be called after retarget-io init.
 ******************************************************************************/
void config_print_boot_banner(void);

/** @} */  /* end of gateway_config */
