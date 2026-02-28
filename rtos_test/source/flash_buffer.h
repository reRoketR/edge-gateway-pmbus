/**
 * @file flash_buffer.h
 * @brief Flash-backed persistent ring buffer for offline store-and-forward.
 * @ingroup buffer_mgr
 *
 * @details
 * Uses the PSoC 6 Em_EEPROM flash region (32 KB at 0x14000000) as a
 * persistent ring buffer.  Records survive power-cycle and reboot.
 *
 * Layout (in Em_EEPROM flash, 32 KB = 64 × 512-byte rows):
 *
 *   Row 0        : Metadata row (head/tail pointers, magic, CRC)
 *   Rows 1..63   : Data rows (one buffer_record_t per row)
 *                   → max 63 records persistent storage
 *
 * Each 512-byte data row stores:
 *   - 4-byte magic (0xB1F0DA7A)
 *   - 2-byte payload_len
 *   - 2-byte reserved
 *   - 80-byte topic
 *   - 420-byte payload (truncated from 512 to fit)
 *   - 4-byte CRC32 of the above
 *
 * Flash API used:
 *   - Cy_Flash_WriteRow() — blocking, pre-program + erase + write
 *   - Cy_Flash_EraseRow() — blocking, erase a single 512-byte row
 *   - Direct pointer reads (flash is memory-mapped)
 *
 * Constraints:
 *   - Flash writes block ~16–20 ms. Only called from buffer_task (Task C)
 *     which runs at low priority.
 *   - Interrupts must remain enabled during flash writes (PDL requirement).
 *   - Em_EEPROM region is in a separate flash sector, avoiding Read-while-
 *     Write violations with application code in main flash.
 *   - Flash endurance: ~100 K cycles per row. With 63 data rows and round-
 *     robin writes, effective endurance is ~6.3 M records before wear-out.
 *
 * @see docs/persistent_buffer.md
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "buffer_mgr.h"     /* buffer_record_t */

/*******************************************************************************
 * Flash region constants
 ******************************************************************************/

/** Em_EEPROM flash base address (PSoC 62, see device datasheet) */
#define FLASH_BUF_BASE_ADDR     (0x14000000UL)

/** Em_EEPROM region size (32 KB) */
#define FLASH_BUF_REGION_SIZE   (0x00008000UL)

/** Flash row size (PSoC 6 = 512 bytes) */
#define FLASH_BUF_ROW_SIZE      (512U)

/** Total rows in Em_EEPROM region */
#define FLASH_BUF_TOTAL_ROWS    (FLASH_BUF_REGION_SIZE / FLASH_BUF_ROW_SIZE)  /* 64 */

/** Row 0 is reserved for metadata; data rows start at row 1 */
#define FLASH_BUF_META_ROW      (0U)
#define FLASH_BUF_DATA_ROW_BASE (1U)

/** Maximum data rows available for record storage */
#define FLASH_BUF_MAX_DATA_ROWS (FLASH_BUF_TOTAL_ROWS - 1U)   /* 63 */

/*******************************************************************************
 * On-flash record format (fits in one 512-byte row)
 ******************************************************************************/

/** Magic value identifying a valid flash record */
#define FLASH_RECORD_MAGIC      (0xB1F0DA7AUL)

/** Max payload size in flash record (512 - 8 header - 80 topic - 4 CRC) */
#define FLASH_PAYLOAD_MAX       (420U)

/** On-flash data record (exactly 512 bytes, packed into one row) */
typedef struct __attribute__((packed)) {
    uint32_t magic;                          /**< FLASH_RECORD_MAGIC          */
    uint16_t payload_len;                    /**< Actual payload length       */
    uint16_t reserved;                       /**< Padding / future use        */
    char     topic[BUFFER_TOPIC_MAX];        /**< MQTT topic (80 bytes)       */
    char     payload[FLASH_PAYLOAD_MAX];     /**< JSON payload (420 bytes)    */
    uint32_t crc32;                          /**< CRC32 of bytes 0..507       */
} flash_data_row_t;

/* Static assert: ensure the row fits exactly in one flash row */
_Static_assert(sizeof(flash_data_row_t) == FLASH_BUF_ROW_SIZE,
               "flash_data_row_t must be exactly 512 bytes");

/*******************************************************************************
 * On-flash metadata row (row 0)
 ******************************************************************************/

/** Magic value for the metadata row */
#define FLASH_META_MAGIC        (0x4D455441UL)  /* "META" in ASCII */

/** Metadata row layout (512 bytes, first 32 used, rest 0xFF) */
typedef struct __attribute__((packed)) {
    uint32_t magic;              /**< FLASH_META_MAGIC                       */
    uint16_t head;               /**< Next write slot (0..MAX_DATA_ROWS-1)   */
    uint16_t tail;               /**< Next read slot  (0..MAX_DATA_ROWS-1)   */
    uint16_t count;              /**< Number of valid records                */
    uint16_t version;            /**< Metadata format version (1)            */
    uint32_t total_writes;       /**< Cumulative write counter (wear metric) */
    uint32_t crc32;              /**< CRC32 of bytes 0..15                   */
    uint8_t  _pad[FLASH_BUF_ROW_SIZE - 20]; /**< Pad to 512 bytes           */
} flash_meta_row_t;

_Static_assert(sizeof(flash_meta_row_t) == FLASH_BUF_ROW_SIZE,
               "flash_meta_row_t must be exactly 512 bytes");

/*******************************************************************************
 * Public API
 ******************************************************************************/

/**
 * @brief Initialise the flash buffer.
 *
 * Reads the metadata row from flash.  If valid (magic + CRC match),
 * restores head/tail/count.  If invalid (first boot or corrupted),
 * erases the region and writes a fresh metadata row.
 *
 * @return true on success, false on flash I/O error.
 */
bool flash_buffer_init(void);

/**
 * @brief Write a record to flash.
 *
 * Writes to the next available data row.  If full:
 *   - drop_oldest = true  → overwrites oldest (advances tail)
 *   - drop_oldest = false → drops new record
 *
 * Also writes an updated metadata row (for crash recovery).
 *
 * @warning This function calls Cy_Flash_WriteRow() which blocks for ~16 ms.
 *          Call only from a low-priority task.
 *
 * @param[in] topic        MQTT topic string
 * @param[in] payload      JSON payload string
 * @param[in] payload_len  Payload length
 *
 * @return true if the record was written, false on error or drop.
 */
bool flash_buffer_put(const char *topic, const char *payload, uint16_t payload_len);

/**
 * @brief Read (peek) the oldest record from flash without consuming it.
 *
 * Reads directly from flash (memory-mapped, no copy needed for validation).
 *
 * @param[out] out  Record to fill (copies topic + payload to caller's buffer)
 *
 * @return true if a record was read, false if flash buffer is empty.
 */
bool flash_buffer_peek(buffer_record_t *out);

/**
 * @brief Consume (remove) the oldest record from flash.
 *
 * Advances the tail pointer and writes updated metadata.
 *
 * @return true on success, false if empty or flash write error.
 */
bool flash_buffer_consume(void);

/**
 * @brief Get the current number of records in flash.
 *
 * @return Number of flash-buffered records.
 */
uint32_t flash_buffer_depth(void);

/**
 * @brief Get cumulative flash write count (for wear monitoring).
 *
 * @return Total number of row writes since first initialisation.
 */
uint32_t flash_buffer_total_writes(void);

/**
 * @brief Erase the entire flash buffer region and reset metadata.
 *
 * Useful for factory reset or test cleanup.
 *
 * @return true on success, false on erase error.
 */
bool flash_buffer_erase_all(void);
