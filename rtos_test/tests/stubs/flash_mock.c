/**
 * @file flash_mock.c
 * @brief RAM-backed Em_EEPROM mock implementation for persistent_seq tests.
 */

#include "flash_mock.h"
#include "flash_buffer.h"
#include <string.h>

/*******************************************************************************
 * Private data — 32 KB RAM array (same size as Em_EEPROM region)
 ******************************************************************************/
static uint8_t s_flash[FLASH_BUF_TOTAL_ROWS * FLASH_BUF_ROW_SIZE];

/*******************************************************************************
 * Bank helpers
 ******************************************************************************/
static uint8_t *bank_ptr(int bank)
{
    uint32_t row = (bank == 0) ? PERSISTENT_SEQ_ROW_A : PERSISTENT_SEQ_ROW_B;
    return &s_flash[row * FLASH_BUF_ROW_SIZE];
}

/*******************************************************************************
 * Public API
 ******************************************************************************/
void flash_mock_reset(void)
{
    memset(s_flash, 0xFF, sizeof(s_flash));
}

void flash_mock_corrupt_bank(int bank)
{
    /* Flip a byte in the magic field to invalidate CRC and magic */
    uint8_t *p = bank_ptr(bank);
    p[0] ^= 0xFFu;
}

const persistent_seq_bank_t *flash_mock_bank_read(int bank)
{
    return (const persistent_seq_bank_t *)bank_ptr(bank);
}

bool flash_mock_bank_write(int bank, const persistent_seq_bank_t *data)
{
    memcpy(bank_ptr(bank), data, sizeof(persistent_seq_bank_t));
    return true;
}

uint8_t *flash_mock_raw(void)
{
    return s_flash;
}
