/**
 * @file flash_mock.h
 * @brief RAM-backed Em_EEPROM mock for persistent_seq host tests.
 */
#ifndef FLASH_MOCK_H
#define FLASH_MOCK_H

#include "persistent_seq.h"
#include <stdint.h>
#include <stdbool.h>

/** Reset mock flash to all-0xFF (erased state). */
void flash_mock_reset(void);

/** Corrupt a specific bank so CRC/magic validation fails. */
void flash_mock_corrupt_bank(int bank);

/** Read pointer to bank data (bank 0 = row A, bank 1 = row B). */
const persistent_seq_bank_t *flash_mock_bank_read(int bank);

/** Write bank data (returns true on success). */
bool flash_mock_bank_write(int bank, const persistent_seq_bank_t *data);

/** Raw access to mock flash for advanced corruption tests. */
uint8_t *flash_mock_raw(void);

#endif /* FLASH_MOCK_H */
