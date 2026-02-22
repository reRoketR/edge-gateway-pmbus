/**
 * @file pmbus_master.h
 * @brief PMBus/SMBus master driver for the gateway.
 * @ingroup pmbus_master
 *
 * @details
 * Uses PDL SCB I2C master API (not HAL) on the PMBUS_CONTROLLER peripheral
 * (SCB3, P6_0 SCL / P6_1 SDA).
 *
 * Provides:
 *   - Initialization and de-initialization
 *   - SMBus Read Word (write cmd → restart → read 2 bytes)
 *   - SMBus Read Byte (write cmd → restart → read 1 byte)
 *   - SMBus Read Block (write cmd → restart → read N bytes)
 *   - SMBus Send Byte (write cmd byte only)
 *   - Optional PEC (CRC-8, polynomial 0x07) verification
 *   - Retry logic with configurable timeout and attempt count
 *   - Bus recovery (9 SCL clock toggles)
 *
 * @see agent.md §3, §6
 *
 * @defgroup pmbus_master PMBus Master Driver
 * @brief I²C/SMBus low-level PMBus master with PEC and retry support.
 * @{
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "cy_result.h"

/*******************************************************************************
 * Return codes
 ******************************************************************************/
typedef enum {
    PMBUS_OK            = 0,    /**< Transaction completed successfully       */
    PMBUS_ERR_NACK      = 1,    /**< Target NACKed (address or data)          */
    PMBUS_ERR_TIMEOUT   = 2,    /**< Transaction timed out                    */
    PMBUS_ERR_BUS       = 3,    /**< Bus error / arbitration lost             */
    PMBUS_ERR_PEC       = 4,    /**< PEC (CRC-8) mismatch                    */
    PMBUS_ERR_ARG       = 5,    /**< Invalid argument                        */
    PMBUS_ERR_NOT_INIT  = 6,    /**< Driver not initialized                  */
} pmbus_status_t;

/*******************************************************************************
 * Initialization / De-initialization
 ******************************************************************************/

/**
 * @brief Initialize the PMBus master driver.
 *
 * Configures the SCB3 (PMBUS_CONTROLLER) block as I2C master using PDL,
 * sets the data rate from g_config.i2c.speed_hz, assigns the clock divider,
 * and enables the SCB interrupt.
 *
 * Must be called once before any pmbus_read_* or pmbus_write_* calls.
 * Uses configuration from g_config.i2c (speed_hz, timeout_ms, etc.).
 *
 * @return PMBUS_OK on success, PMBUS_ERR_BUS on failure.
 */
pmbus_status_t pmbus_init(void);

/**
 * @brief De-initialize the PMBus master driver.
 *
 * Disables the SCB3 block and the interrupt.
 */
void pmbus_deinit(void);

/*******************************************************************************
 * SMBus Transactions
 ******************************************************************************/

/**
 * @brief SMBus Read Word — reads 2 data bytes from a PMBus command register.
 *
 * Protocol on the wire:
 *   [S][addr+W][cmd][Sr][addr+R][low][high][P]
 *   (with optional PEC byte appended if pec_enabled)
 *
 * @param[in]  addr_7bit  7-bit target address
 * @param[in]  cmd        PMBus command code
 * @param[out] out_word   Pointer to store the 16-bit result (little-endian)
 *
 * @return PMBUS_OK on success, error code otherwise.
 *         Retries and timeout are applied per g_config.i2c settings.
 */
pmbus_status_t pmbus_read_word(uint8_t addr_7bit, uint8_t cmd, uint16_t *out_word);

/**
 * @brief SMBus Read Byte — reads a single data byte from a command.
 *
 * Protocol on the wire:
 *   [S][addr+W][cmd][Sr][addr+R][data][P]
 *   (with optional PEC byte appended if pec_enabled)
 *
 * Used for single-byte PMBus commands such as VOUT_MODE (0x20).
 *
 * @param[in]  addr_7bit  7-bit target address
 * @param[in]  cmd        PMBus command code
 * @param[out] out_byte   Pointer to store the 8-bit result
 *
 * @return PMBUS_OK on success, error code otherwise.
 */
pmbus_status_t pmbus_read_byte(uint8_t addr_7bit, uint8_t cmd, uint8_t *out_byte);

/**
 * @brief SMBus Read Block — reads a variable-length block from a command.
 *
 * Protocol on the wire:
 *   [S][addr+W][cmd][Sr][addr+R][byte_count][data0..dataN-1][P]
 *   (with optional PEC byte appended if pec_enabled)
 *
 * @param[in]  addr_7bit  7-bit target address
 * @param[in]  cmd        PMBus command code
 * @param[out] buf        Buffer to store received data (excluding byte_count)
 * @param[in]  buf_size   Max bytes to read
 * @param[out] out_len    Actual bytes received (from byte_count field)
 *
 * @return PMBUS_OK on success, error code otherwise.
 */
pmbus_status_t pmbus_read_block(uint8_t addr_7bit, uint8_t cmd,
                                uint8_t *buf, uint8_t buf_size,
                                uint8_t *out_len);

/**
 * @brief SMBus Send Byte — writes a single command byte (no data).
 *
 * Protocol on the wire:
 *   [S][addr+W][cmd][P]
 *
 * Used for commands like CLEAR_FAULTS.
 *
 * @param[in]  addr_7bit  7-bit target address
 * @param[in]  cmd        PMBus command code
 *
 * @return PMBUS_OK on success, error code otherwise.
 */
pmbus_status_t pmbus_send_byte(uint8_t addr_7bit, uint8_t cmd);

/*******************************************************************************
 * Bus Recovery
 ******************************************************************************/

/**
 * @brief Attempt I2C bus recovery by toggling SCL 9 times.
 *
 * If SDA is stuck low (target holding the bus), toggling SCL can release it.
 * Called automatically by the retry logic when bus_recovery is enabled in config.
 *
 * @return PMBUS_OK if SDA is released, PMBUS_ERR_BUS if still stuck.
 */
pmbus_status_t pmbus_bus_recovery(void);

/*******************************************************************************
 * PEC utility (exposed for unit testing)
 ******************************************************************************/

/**
 * @brief Compute SMBus PEC (CRC-8) over a byte buffer.
 *
 * Uses polynomial 0x07 (x^8 + x^2 + x + 1), initial value 0x00.
 *
 * @param[in]  data  Input bytes
 * @param[in]  len   Number of bytes
 *
 * @return Computed CRC-8 value.
 */
uint8_t pmbus_crc8(const uint8_t *data, uint8_t len);

/** @} */  /* end of pmbus_master */
