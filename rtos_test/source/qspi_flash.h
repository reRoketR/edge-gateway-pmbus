/*******************************************************************************
 * File Name:   qspi_flash.h
 *
 * Description: QSPI external flash (S25FL512S) initialization and self-test.
 *              Part of D2a-1: QSPI HAL Bring-Up & Verification.
 *
 ******************************************************************************/
#ifndef QSPI_FLASH_H
#define QSPI_FLASH_H

#include "cy_result.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/**
 * @brief Initialize the QSPI flash peripheral via the serial-flash library.
 *        Prints JEDEC info (chip name, total size, sector size) to UART.
 * @return CY_RSLT_SUCCESS on success, error code otherwise.
 */
cy_rslt_t qspi_flash_init(void);

/**
 * @brief Run a write/read/erase self-test on a dedicated test sector.
 *        Uses the LAST sector of the flash to avoid collision with data.
 * @return true if the self-test passed, false otherwise.
 */
bool qspi_flash_self_test(void);

/**
 * @brief Return the total flash size in bytes (populated after init).
 */
size_t qspi_flash_get_size(void);

/**
 * @brief Return the erase sector size in bytes (populated after init).
 */
size_t qspi_flash_get_erase_size(void);

#endif /* QSPI_FLASH_H */
