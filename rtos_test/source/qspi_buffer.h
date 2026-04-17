/**
 * @file qspi_buffer.h
 * @brief External QSPI flash-backed persistent ring buffer.
 * @ingroup buffer_mgr
 *
 * @details
 * Uses the external S25FL512S NOR flash as a high-capacity persistent ring buffer.
 * Allocates a 2 MB region starting at 0x00000000.
 *
 * Architecture:
 *   - Region Size: 2 MB (8 sectors × 256 KB)
 *   - Sector 0-1: Metadata Journal Ping-Pong (wear leveling & power-safe rollover)
 *   - Sectors 2-7: Data Ring Buffer (records appended sequentially, wrapped)
 *
 * @see docs/persistent_buffer.md
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "buffer_mgr.h"     /* buffer_record_t */

/*******************************************************************************
 * QSPI Flash Region Constants
 ******************************************************************************/

/** Flash start address for the buffer region (0x00000000 in QSPI space) */
#define QSPI_BUF_REGION_START     (0x00000000UL)

/** Flash sector size (256 KB for S25FL512S) */
#define QSPI_BUF_SECTOR_SIZE      (262144UL)

/** Total region size (2 MB) */
#define QSPI_BUF_REGION_SIZE      (2097152UL)

/** Total sectors in the allocated region */
#define QSPI_BUF_TOTAL_SECTORS    (QSPI_BUF_REGION_SIZE / QSPI_BUF_SECTOR_SIZE) /* 8 */

/** Journal mapping: Sectors 0 and 1 */
#define QSPI_BUF_JOURNAL_0_OFFSET (0UL)
#define QSPI_BUF_JOURNAL_1_OFFSET (QSPI_BUF_SECTOR_SIZE)

/** Data Region boundaries: Sectors 2 to 7 */
#define QSPI_BUF_DATA_START       (2UL * QSPI_BUF_SECTOR_SIZE)
#define QSPI_BUF_DATA_SECTORS     (QSPI_BUF_TOTAL_SECTORS - 2U) /* 6 sectors */

/** Memory-mapped base address of the QSPI memory in the PSoC 6 address space */
#ifndef QSPI_MEM_MAPPED_BASE
#ifdef QSPI_BUF_HOST_TEST
#include "qspi_mock.h"
#define QSPI_MEM_MAPPED_BASE      ((uintptr_t)qspi_mock_mmap_base())
#else
#define QSPI_MEM_MAPPED_BASE      (0x18000000UL)
#endif
#endif

/*******************************************************************************
 * On-flash Record Format
 ******************************************************************************/

/** Magic value identifying a valid QSPI data record ("RECD") */
#define QSPI_RECORD_MAGIC         (0x52454344UL)

/** On-flash data record header. */
typedef struct __attribute__((packed)) {
    uint32_t magic;                          /**< QSPI_RECORD_MAGIC           */
    uint16_t payload_len;                    /**< Actual payload length       */
    uint8_t  topic_len;                      /**< Actual topic length         */
    uint8_t  reserved;                       /**< Padding / future use        */
    uint32_t origin_read_start_ms;           /**< Same-boot latency origin    */
    uint32_t origin_boot_gen;                /**< Boot generation marker      */
    /* Followed by: topic bytes, payload bytes, 4-byte CRC32 */
} qspi_data_header_t;

/*******************************************************************************
 * On-flash Metadata Journal
 ******************************************************************************/

/** Magic value for the metadata entry ("MET2") */
#define QSPI_META_MAGIC           (0x4D455432UL)

/**
 * Metadata journal entry.
 * Length: 28 bytes.
 * Capacity in 1 sector: 9362 entries.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;              /**< QSPI_META_MAGIC                        */
    uint32_t seq;                /**< Incrementing sequence number           */
    uint32_t head_offset;        /**< Absolute QSPI offset for next write    */
    uint32_t tail_offset;        /**< Absolute QSPI offset for oldest read   */
    uint32_t count;              /**< Number of valid records in ring        */
    uint32_t total_writes;       /**< Lifetime record write counter          */
    uint32_t crc32;              /**< CRC32 of bytes 0..23                   */
} qspi_meta_entry_t;

_Static_assert(sizeof(qspi_meta_entry_t) == 28, "qspi_meta_entry_t must be 28 bytes");


/*******************************************************************************
 * Public API
 ******************************************************************************/

bool qspi_buffer_init(void);
bool qspi_buffer_put_record(const buffer_record_t *rec);
bool qspi_buffer_put(const char *topic, const char *payload, uint16_t payload_len);
bool qspi_buffer_peek(buffer_record_t *out);
bool qspi_buffer_consume(void);
uint32_t qspi_buffer_depth(void);
uint32_t qspi_buffer_total_writes(void);
bool qspi_buffer_erase_all(void);
void qspi_buffer_lock(void);
void qspi_buffer_unlock(void);
