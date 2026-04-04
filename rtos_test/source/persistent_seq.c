/*******************************************************************************
 * File Name:   persistent_seq.c
 *
 * Description: Persistent sequence counter stored in Em_EEPROM rows 62–63
 *              using A/B ping-pong banks with CRC-32 validation.
 *
 *              On boot: read both banks → pick higher seq → bump boot_count
 *              → write to alternate bank.
 *
 *              During runtime: checkpoint(seq) writes to the next bank in
 *              the ping-pong rotation.
 *
 * Related Document: docs/persistent_buffer.md, persistent_seq.h
 *
 ******************************************************************************/

#include "persistent_seq.h"

#ifndef PERSISTENT_SEQ_HOST_TEST
#include "cy_flash.h"
#include "cy_syslib.h"
#endif

#include <stdio.h>
#include <string.h>

/*******************************************************************************
 * CRC-32 (ISO 3309 — same polynomial as flash_buffer.c, qspi_buffer.c)
 ******************************************************************************/
static uint32_t crc32_calc(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFUL;

    for (size_t i = 0; i < len; i++)
    {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++)
        {
            if (crc & 1u)
                crc = (crc >> 1u) ^ 0xEDB88320UL;
            else
                crc >>= 1u;
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}

/*******************************************************************************
 * Private state
 ******************************************************************************/

/** Cached sequence value (RAM shadow) */
static uint32_t s_seq;

/** Cached boot count */
static uint32_t s_boot_count;

/** Next bank to write (0 = A, 1 = B) — alternates on each write */
static int s_next_bank;

/*******************************************************************************
 * Flash address helpers
 ******************************************************************************/

static inline uint32_t bank_addr(int bank)
{
    uint32_t row = (bank == 0) ? PERSISTENT_SEQ_ROW_A : PERSISTENT_SEQ_ROW_B;
    return FLASH_BUF_BASE_ADDR + (row * FLASH_BUF_ROW_SIZE);
}

/*******************************************************************************
 * Flash I/O
 ******************************************************************************/

#ifndef PERSISTENT_SEQ_HOST_TEST

/** Read a bank via memory-mapped pointer (Em_EEPROM is in address space) */
static const persistent_seq_bank_t *bank_read(int bank)
{
    return (const persistent_seq_bank_t *)bank_addr(bank);
}

/** Write a 512-byte bank to flash (blocking ~16 ms) */
static bool bank_write(int bank, const persistent_seq_bank_t *data)
{
    uint32_t addr = bank_addr(bank);
    cy_en_flashdrv_status_t status =
        Cy_Flash_WriteRow(addr, (const uint32_t *)data);
    if (status != CY_FLASH_DRV_SUCCESS)
    {
        printf("[SEQ] ERROR: WriteRow @ 0x%08lX failed, status=0x%08lX\n",
               (unsigned long)addr, (unsigned long)status);
        return false;
    }
    Cy_SysLib_ClearFlashCacheAndBuffer();
    return true;
}

#else /* PERSISTENT_SEQ_HOST_TEST — host-side test stubs (flash_mock.c) */

extern const persistent_seq_bank_t *flash_mock_bank_read(int bank);
extern bool flash_mock_bank_write(int bank, const persistent_seq_bank_t *data);

static const persistent_seq_bank_t *bank_read(int bank)
{
    return flash_mock_bank_read(bank);
}

static bool bank_write(int bank, const persistent_seq_bank_t *data)
{
    return flash_mock_bank_write(bank, data);
}

#endif /* PERSISTENT_SEQ_HOST_TEST */

/*******************************************************************************
 * Bank validation
 ******************************************************************************/

/** Validate a bank: check magic and CRC-32 of first 12 bytes */
static bool bank_valid(const persistent_seq_bank_t *b)
{
    if (b->magic != PERSISTENT_SEQ_MAGIC)
    {
        return false;
    }
    uint32_t expected_crc = crc32_calc(b, offsetof(persistent_seq_bank_t, crc32));
    return (b->crc32 == expected_crc);
}

/*******************************************************************************
 * Prepare a bank structure for writing
 ******************************************************************************/

static void bank_prepare(persistent_seq_bank_t *buf, uint32_t seq,
                         uint32_t boot_count)
{
    memset(buf, 0xFF, sizeof(*buf));
    buf->magic      = PERSISTENT_SEQ_MAGIC;
    buf->seq_value  = seq;
    buf->boot_count = boot_count;
    buf->crc32      = crc32_calc(buf, offsetof(persistent_seq_bank_t, crc32));
}

/*******************************************************************************
 * Public API
 ******************************************************************************/

bool persistent_seq_init(void)
{
    const persistent_seq_bank_t *a = bank_read(0);
    const persistent_seq_bank_t *b = bank_read(1);

    bool a_ok = bank_valid(a);
    bool b_ok = bank_valid(b);

    uint32_t seq;
    uint32_t boot;
    int source_bank;   /* bank we recovered from, -1 if fresh */

    if (a_ok && b_ok)
    {
        /* Both valid — pick the one with the higher seq.
         * Handle wrap-around: if difference > 0x80000000, the "lower"
         * value has actually wrapped past the "higher" one.
         * Tie-break on equal seq: pick the higher boot_count (the more
         * recent write). Without this, repeated boots with no traffic
         * would stall boot_count by always picking the older bank. */
        int32_t diff = (int32_t)(a->seq_value - b->seq_value);
        if (diff > 0)
        {
            seq  = a->seq_value;
            boot = a->boot_count;
            source_bank = 0;
        }
        else if (diff < 0)
        {
            seq  = b->seq_value;
            boot = b->boot_count;
            source_bank = 1;
        }
        else
        {
            /* Equal seq — pick the bank with the higher boot_count */
            if (a->boot_count >= b->boot_count)
            {
                seq  = a->seq_value;
                boot = a->boot_count;
                source_bank = 0;
            }
            else
            {
                seq  = b->seq_value;
                boot = b->boot_count;
                source_bank = 1;
            }
        }
    }
    else if (a_ok)
    {
        seq  = a->seq_value;
        boot = a->boot_count;
        source_bank = 0;
    }
    else if (b_ok)
    {
        seq  = b->seq_value;
        boot = b->boot_count;
        source_bank = 1;
    }
    else
    {
        /* First boot or double corruption */
        seq  = 0u;
        boot = 0u;
        source_bank = -1;
    }

    /* Bump boot count */
    boot++;

    /* Cache in RAM */
    s_seq        = seq;
    s_boot_count = boot;

    /* Write to the OPPOSITE bank (ping-pong) */
    int write_bank = (source_bank <= 0) ? 1 : 0;
    persistent_seq_bank_t buf;
    bank_prepare(&buf, seq, boot);

    if (!bank_write(write_bank, &buf))
    {
        printf("[SEQ] WARNING: init write to bank %d failed\n", write_bank);
        return false;
    }

    /* Next checkpoint goes to the other bank */
    s_next_bank = (write_bank == 0) ? 1 : 0;

    if (source_bank >= 0)
    {
        printf("[SEQ] Restored: seq=%lu boot_count=%lu (from bank %c)\n",
               (unsigned long)seq, (unsigned long)boot,
               (source_bank == 0) ? 'A' : 'B');
    }
    else
    {
        printf("[SEQ] Fresh init: seq=0 boot_count=%lu\n",
               (unsigned long)boot);
    }

    return true;
}

uint32_t persistent_seq_get(void)
{
    return s_seq;
}

uint32_t persistent_seq_get_boot_count(void)
{
    return s_boot_count;
}

void persistent_seq_checkpoint(uint32_t seq)
{
    s_seq = seq;

    persistent_seq_bank_t buf;
    bank_prepare(&buf, seq, s_boot_count);

    if (!bank_write(s_next_bank, &buf))
    {
        printf("[SEQ] WARNING: checkpoint to bank %d failed\n", s_next_bank);
        return;
    }

    /* Alternate for next write */
    s_next_bank = (s_next_bank == 0) ? 1 : 0;
}
