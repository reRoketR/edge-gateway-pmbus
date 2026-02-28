/*******************************************************************************
 * File Name:   flash_buffer.c
 *
 * Description: Flash-backed persistent ring buffer using PSoC 6 Em_EEPROM
 *              flash region (32 KB at 0x14000000).
 *
 *              Uses Cy_Flash_WriteRow() (blocking, ~16 ms) to persist records
 *              and metadata.  Records survive power-cycle and reboot.
 *
 *              Flash row size = 512 bytes.  Row 0 = metadata, Rows 1..63 = data.
 *
 * Related Document: docs/persistent_buffer.md, agent.md §8
 *
 ******************************************************************************/

#include "flash_buffer.h"
#include "gateway_config.h"
#include "metrics.h"

#include "cy_flash.h"
#include "cy_syslib.h"

#include <stdio.h>
#include <string.h>

/*******************************************************************************
 * CRC-32 (ISO 3309 / ITU-T V.42 — same as zlib/PNG)
 *
 * Small table-less implementation suitable for embedded use.
 * Not speed-critical: only called on flash writes (slow path anyway).
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
 * Private state (RAM shadow of metadata)
 ******************************************************************************/

/** RAM copy of flash metadata (updated on every put/consume) */
static flash_meta_row_t s_meta;

/** True after successful flash_buffer_init() */
static bool s_initialised = false;

/*******************************************************************************
 * Flash address helpers
 ******************************************************************************/

/** Get the flash address of a given row index (0..63) */
static inline uint32_t row_addr(uint32_t row_index)
{
    return FLASH_BUF_BASE_ADDR + (row_index * FLASH_BUF_ROW_SIZE);
}

/** Get the flash address for a data slot (0..62) → row (1..63) */
static inline uint32_t data_slot_addr(uint16_t slot)
{
    return row_addr((uint32_t)FLASH_BUF_DATA_ROW_BASE + slot);
}

/*******************************************************************************
 * Flash I/O wrappers
 ******************************************************************************/

/**
 * @brief Write a 512-byte row to flash (blocking).
 *
 * Cy_Flash_WriteRow performs pre-program + erase + program in one call.
 *
 * @param addr  Row-aligned flash address
 * @param data  Pointer to 512 bytes of data (must be 4-byte aligned)
 * @return true on success
 */
static bool flash_write_row(uint32_t addr, const uint32_t *data)
{
    cy_en_flashdrv_status_t status = Cy_Flash_WriteRow(addr, data);
    if (status != CY_FLASH_DRV_SUCCESS)
    {
        printf("[FLASH] ERROR: WriteRow @ 0x%08lX failed, status=0x%08lX\n",
               (unsigned long)addr, (unsigned long)status);
        return false;
    }
    /* Clear cache after flash write to ensure consistent reads */
    Cy_SysLib_ClearFlashCacheAndBuffer();
    return true;
}

/**
 * @brief Erase a single 512-byte row (blocking).
 */
static bool flash_erase_row(uint32_t addr)
{
    cy_en_flashdrv_status_t status = Cy_Flash_EraseRow(addr);
    if (status != CY_FLASH_DRV_SUCCESS)
    {
        printf("[FLASH] ERROR: EraseRow @ 0x%08lX failed, status=0x%08lX\n",
               (unsigned long)addr, (unsigned long)status);
        return false;
    }
    Cy_SysLib_ClearFlashCacheAndBuffer();
    return true;
}

/*******************************************************************************
 * Metadata persistence
 ******************************************************************************/

/**
 * @brief Write the current s_meta to flash row 0.
 */
static bool meta_write(void)
{
    /* Update CRC over the first 20 bytes (magic through total_writes) */
    s_meta.crc32 = crc32_calc(&s_meta, offsetof(flash_meta_row_t, crc32));

    return flash_write_row(row_addr(FLASH_BUF_META_ROW),
                           (const uint32_t *)&s_meta);
}

/**
 * @brief Read and validate the metadata row from flash.
 *
 * @return true if metadata is valid (magic + CRC match)
 */
static bool meta_read_and_validate(void)
{
    /* Flash is memory-mapped — read directly via pointer */
    const flash_meta_row_t *flash_meta =
        (const flash_meta_row_t *)row_addr(FLASH_BUF_META_ROW);

    /* Check magic */
    if (flash_meta->magic != FLASH_META_MAGIC)
    {
        return false;
    }

    /* Check version */
    if (flash_meta->version != 1u)
    {
        return false;
    }

    /* Validate CRC */
    uint32_t expected_crc = crc32_calc(flash_meta,
                                       offsetof(flash_meta_row_t, crc32));
    if (flash_meta->crc32 != expected_crc)
    {
        return false;
    }

    /* Sanity check pointers */
    if (flash_meta->head >= FLASH_BUF_MAX_DATA_ROWS ||
        flash_meta->tail >= FLASH_BUF_MAX_DATA_ROWS ||
        flash_meta->count > FLASH_BUF_MAX_DATA_ROWS)
    {
        return false;
    }

    /* Copy to RAM shadow */
    memcpy(&s_meta, flash_meta, sizeof(flash_meta_row_t));
    return true;
}

/**
 * @brief Initialise metadata to empty state and write to flash.
 */
static bool meta_init_fresh(void)
{
    memset(&s_meta, 0xFF, sizeof(s_meta));  /* Fill padding with 0xFF (erased state) */
    s_meta.magic        = FLASH_META_MAGIC;
    s_meta.head         = 0u;
    s_meta.tail         = 0u;
    s_meta.count        = 0u;
    s_meta.version      = 1u;
    s_meta.total_writes = 0u;

    return meta_write();
}

/*******************************************************************************
 * Data record helpers
 ******************************************************************************/

/**
 * @brief Validate a data record read from flash.
 */
static bool record_is_valid(const flash_data_row_t *rec)
{
    if (rec->magic != FLASH_RECORD_MAGIC)
    {
        return false;
    }

    if (rec->payload_len > FLASH_PAYLOAD_MAX)
    {
        return false;
    }

    /* CRC covers everything except the last 4 bytes (crc32 field) */
    uint32_t expected = crc32_calc(rec, offsetof(flash_data_row_t, crc32));
    return (rec->crc32 == expected);
}

/*******************************************************************************
 * Public API
 ******************************************************************************/

bool flash_buffer_init(void)
{
    uint32_t max_records = g_config.buffer.flash_max_records;
    if (max_records == 0u)
    {
        printf("[FLASH] Flash buffer disabled (flash_max_records=0)\n");
        s_initialised = false;
        return true;  /* Not an error */
    }

    printf("[FLASH] Initialising flash buffer (Em_EEPROM @ 0x%08lX, %lu rows)\n",
           (unsigned long)FLASH_BUF_BASE_ADDR,
           (unsigned long)FLASH_BUF_MAX_DATA_ROWS);

    /* Try to read existing metadata */
    if (meta_read_and_validate())
    {
        /* Clamp head/tail/count to the configured capacity.
         * If flash_max_records was reduced between builds, stale metadata
         * could point beyond the valid range.  Clamping prevents OOB access. */
        uint16_t cap = FLASH_BUF_MAX_DATA_ROWS;
        if (g_config.buffer.flash_max_records < cap)
        {
            cap = (uint16_t)g_config.buffer.flash_max_records;
        }

        bool clamped = false;
        if (s_meta.head >= cap) { s_meta.head = 0u; clamped = true; }
        if (s_meta.tail >= cap) { s_meta.tail = 0u; clamped = true; }
        if (s_meta.count > cap) { s_meta.count = cap; clamped = true; }

        if (clamped)
        {
            printf("[FLASH] WARNING: clamped metadata to capacity=%u "
                   "(head=%u tail=%u count=%u)\n",
                   (unsigned)cap, (unsigned)s_meta.head,
                   (unsigned)s_meta.tail, (unsigned)s_meta.count);
            /* Persist the clamped values */
            (void)meta_write();
        }

        printf("[FLASH] Recovered metadata: head=%u tail=%u count=%u writes=%lu\n",
               (unsigned)s_meta.head, (unsigned)s_meta.tail,
               (unsigned)s_meta.count, (unsigned long)s_meta.total_writes);
        s_initialised = true;
        return true;
    }

    /* First boot or corrupted metadata — initialise fresh */
    printf("[FLASH] No valid metadata found, initialising fresh\n");
    if (!meta_init_fresh())
    {
        printf("[FLASH] ERROR: Failed to write initial metadata\n");
        return false;
    }

    s_initialised = true;
    printf("[FLASH] Flash buffer ready (capacity=%u records)\n",
           (unsigned)FLASH_BUF_MAX_DATA_ROWS);
    return true;
}

bool flash_buffer_put(const char *topic, const char *payload, uint16_t payload_len)
{
    if (!s_initialised || g_config.buffer.flash_max_records == 0u)
    {
        return false;
    }

    /* Cap to available data rows or config limit (whichever is smaller) */
    uint16_t capacity = FLASH_BUF_MAX_DATA_ROWS;
    if (g_config.buffer.flash_max_records < capacity)
    {
        capacity = (uint16_t)g_config.buffer.flash_max_records;
    }

    /* Check if full */
    if (s_meta.count >= capacity)
    {
        if (g_config.buffer.drop_oldest)
        {
            /* Advance tail to drop oldest */
            s_meta.tail = (s_meta.tail + 1u) % capacity;
            s_meta.count--;
            metrics_inc_buffer_dropped();
        }
        else
        {
            metrics_inc_buffer_dropped();
            return false;
        }
    }

    /* Build the data row in a RAM buffer (must be 4-byte aligned) */
    static flash_data_row_t row_buf __attribute__((aligned(4)));
    memset(&row_buf, 0xFF, sizeof(row_buf));  /* Fill with erased state */

    row_buf.magic    = FLASH_RECORD_MAGIC;
    row_buf.reserved = 0u;

    /* Copy topic */
    strncpy(row_buf.topic, topic, BUFFER_TOPIC_MAX - 1u);
    row_buf.topic[BUFFER_TOPIC_MAX - 1u] = '\0';

    /* Copy payload (truncate to FLASH_PAYLOAD_MAX) */
    uint16_t copy_len = payload_len;
    if (copy_len > FLASH_PAYLOAD_MAX - 1u)
    {
        copy_len = FLASH_PAYLOAD_MAX - 1u;
    }
    memcpy(row_buf.payload, payload, copy_len);
    row_buf.payload[copy_len] = '\0';
    row_buf.payload_len = copy_len;

    /* Compute CRC over everything except the crc32 field */
    row_buf.crc32 = crc32_calc(&row_buf, offsetof(flash_data_row_t, crc32));

    /* Write data row to flash */
    uint32_t addr = data_slot_addr(s_meta.head);
    if (!flash_write_row(addr, (const uint32_t *)&row_buf))
    {
        printf("[FLASH] ERROR: Failed to write data row at slot %u\n",
               (unsigned)s_meta.head);
        return false;
    }

    /* Update metadata */
    s_meta.head = (s_meta.head + 1u) % capacity;
    s_meta.count++;
    s_meta.total_writes++;

    /* Persist metadata */
    if (!meta_write())
    {
        printf("[FLASH] WARNING: Data written but metadata update failed\n");
        /* Data is written but metadata is now stale. On reboot the record
         * may be lost because init only trusts the metadata (no row scan).
         * For MVP, accept the risk — meta write failures are rare. */
    }

    metrics_inc_buffer_enqueued();
    return true;
}

bool flash_buffer_peek(buffer_record_t *out)
{
    if (!s_initialised || out == NULL || s_meta.count == 0u)
    {
        return false;
    }

    /* Read data row from flash (memory-mapped) */
    const flash_data_row_t *flash_rec =
        (const flash_data_row_t *)data_slot_addr(s_meta.tail);

    /* Validate */
    if (!record_is_valid(flash_rec))
    {
        printf("[FLASH] WARNING: Invalid record at tail slot %u, "
               "discarding to prevent infinite loop\n",
               (unsigned)s_meta.tail);

        /* Auto-advance tail past the corrupted record so we don't get
         * stuck returning false repeatedly on the same bad slot. */
        uint16_t capacity = FLASH_BUF_MAX_DATA_ROWS;
        if (g_config.buffer.flash_max_records < capacity)
        {
            capacity = (uint16_t)g_config.buffer.flash_max_records;
        }
        s_meta.tail = (s_meta.tail + 1u) % capacity;
        s_meta.count--;
        (void)meta_write();  /* Best-effort persist */
        metrics_inc_buffer_dropped();
        return false;
    }

    /* Copy to caller's buffer_record_t */
    memcpy(out->topic, flash_rec->topic, BUFFER_TOPIC_MAX);
    out->topic[BUFFER_TOPIC_MAX - 1u] = '\0';

    uint16_t len = flash_rec->payload_len;
    if (len > BUFFER_PAYLOAD_MAX - 1u)
    {
        len = BUFFER_PAYLOAD_MAX - 1u;
    }
    memcpy(out->payload, flash_rec->payload, len);
    out->payload[len] = '\0';
    out->payload_len = len;

    return true;
}

bool flash_buffer_consume(void)
{
    if (!s_initialised || s_meta.count == 0u)
    {
        return false;
    }

    uint16_t capacity = FLASH_BUF_MAX_DATA_ROWS;
    if (g_config.buffer.flash_max_records < capacity)
    {
        capacity = (uint16_t)g_config.buffer.flash_max_records;
    }

    /* Advance tail */
    s_meta.tail = (s_meta.tail + 1u) % capacity;
    s_meta.count--;

    /* Persist updated metadata */
    if (!meta_write())
    {
        printf("[FLASH] WARNING: consume metadata update failed\n");
    }

    metrics_inc_buffer_dequeued();
    return true;
}

uint32_t flash_buffer_depth(void)
{
    if (!s_initialised)
    {
        return 0u;
    }
    return s_meta.count;
}

uint32_t flash_buffer_total_writes(void)
{
    if (!s_initialised)
    {
        return 0u;
    }
    return s_meta.total_writes;
}

bool flash_buffer_erase_all(void)
{
    printf("[FLASH] Erasing entire flash buffer region...\n");

    /* Erase all 64 rows */
    for (uint32_t row = 0; row < FLASH_BUF_TOTAL_ROWS; row++)
    {
        if (!flash_erase_row(row_addr(row)))
        {
            printf("[FLASH] ERROR: Failed to erase row %lu\n",
                   (unsigned long)row);
            return false;
        }
    }

    /* Write fresh metadata */
    s_meta.total_writes = 0u;  /* Reset wear counter on full erase */
    if (!meta_init_fresh())
    {
        return false;
    }

    s_initialised = true;
    printf("[FLASH] Flash buffer erased and reinitialised\n");
    return true;
}

/* [] END OF FILE */
