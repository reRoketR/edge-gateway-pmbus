/**
 * @file persistent_seq.h
 * @brief Persistent sequence counter and boot counter (Em_EEPROM A/B banks).
 * @ingroup gateway_ipc
 *
 * @details
 * Stores the global MQTT sequence counter and a boot counter in two
 * ping-pong banks in Em_EEPROM rows 62–63.  On each boot the module
 * reads both banks, validates CRC-32, picks the higher seq, bumps
 * boot_count, and writes the result to the alternate bank.
 *
 * Checkpoint is called periodically (every 100 seq) from
 * gateway_ipc_next_seq() so the counter survives reboots with
 * at most ~100 lost sequence numbers.
 *
 * Flash layout (Em_EEPROM, PSoC 62):
 *   Row 62 : Bank A (512 bytes, 16 used + 496 pad)
 *   Row 63 : Bank B (512 bytes, 16 used + 496 pad)
 *
 * This module does NOT depend on flash_buffer.c and can be used
 * regardless of which persistent buffer backend is active (Em_EEPROM
 * or QSPI).
 *
 * @see flash_buffer.h (row constants), docs/persistent_buffer.md
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "flash_buffer.h"   /* FLASH_BUF_BASE_ADDR, FLASH_BUF_ROW_SIZE */

/*******************************************************************************
 * Constants
 ******************************************************************************/

/** Magic value for a valid persistent seq bank ("SEQ\0") */
#define PERSISTENT_SEQ_MAGIC    (0x53455100UL)

/** Row indices within the Em_EEPROM region */
#define PERSISTENT_SEQ_ROW_A    (62U)
#define PERSISTENT_SEQ_ROW_B    (63U)

/** Checkpoint every N sequence increments (trade-off: persistence vs. wear) */
#define PERSISTENT_SEQ_CHECKPOINT_INTERVAL  (100U)

/*******************************************************************************
 * On-flash bank structure (fits in one 512-byte row)
 ******************************************************************************/

typedef struct __attribute__((packed)) {
    uint32_t magic;          /**< PERSISTENT_SEQ_MAGIC                    */
    uint32_t seq_value;      /**< Last checkpointed sequence number       */
    uint32_t boot_count;     /**< Number of boots seen                    */
    uint32_t crc32;          /**< CRC-32 of bytes 0..11                   */
    uint8_t  _pad[FLASH_BUF_ROW_SIZE - 16]; /**< Pad to 512 bytes (0xFF) */
} persistent_seq_bank_t;

_Static_assert(sizeof(persistent_seq_bank_t) == FLASH_BUF_ROW_SIZE,
               "persistent_seq_bank_t must be exactly 512 bytes");

/*******************************************************************************
 * Public API
 ******************************************************************************/

/**
 * @brief Initialise the persistent sequence counter from Em_EEPROM.
 *
 * Reads banks A and B, validates magic + CRC, picks the bank with the
 * higher seq_value, increments boot_count, and writes the result to the
 * alternate bank (ping-pong).
 *
 * If both banks are invalid (first boot or double corruption), the
 * counter starts at seq=0, boot_count=1.
 *
 * @return true on success (including fresh-init), false on flash I/O error.
 */
bool persistent_seq_init(void);

/**
 * @brief Get the last checkpointed (or restored) sequence number.
 * @return Sequence number cached in RAM.
 */
uint32_t persistent_seq_get(void);

/**
 * @brief Get the current boot count.
 * @return Boot count (1 = first boot).
 */
uint32_t persistent_seq_get_boot_count(void);

/**
 * @brief Checkpoint the current sequence number to flash.
 *
 * Writes to the next ping-pong bank.  Called periodically from
 * gateway_ipc_next_seq() (every PERSISTENT_SEQ_CHECKPOINT_INTERVAL).
 *
 * @param[in] seq  Current sequence number to persist.
 */
void persistent_seq_checkpoint(uint32_t seq);
