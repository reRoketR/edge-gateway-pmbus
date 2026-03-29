#include "qspi_buffer.h"
#include "cy_serial_flash_qspi.h"
#include "cy_utils.h"
#include "cy_syslib.h"
#include "metrics.h"
#include <string.h>
#include <stdio.h>

#ifndef QSPI_BUF_HOST_TEST
#include "FreeRTOS.h"
#include "semphr.h"
static SemaphoreHandle_t s_qspi_mutex = NULL;
#define QSPI_LOCK()    do { if (s_qspi_mutex) xSemaphoreTake(s_qspi_mutex, portMAX_DELAY); } while(0)
#define QSPI_UNLOCK()  do { if (s_qspi_mutex) xSemaphoreGive(s_qspi_mutex); } while(0)
#else
#define QSPI_LOCK()    ((void)0)
#define QSPI_UNLOCK()  ((void)0)
#endif

/*******************************************************************************
 * INTERNAL STATE
 ******************************************************************************/

static qspi_meta_entry_t s_meta_cache;
static uint32_t          s_meta_index = 0;
static uint32_t          s_meta_active_sector = 0; // 0 or 1
static bool              s_is_initialized = false;

// Forward declaration
static bool qspi_buffer_consume_internal(bool is_drop);

static bool qspi_set_xip_enabled(bool enable)
{
#if defined(QSPI_BUF_HOST_TEST)
    (void)enable;
    return true;
#else
    cy_rslt_t result = cy_serial_flash_qspi_enable_xip(enable);
    if (result != CY_RSLT_SUCCESS) {
        printf("[QSPI_BUF] WARNING: failed to %s XIP (0x%08lX)\n",
               enable ? "enable" : "disable",
               (unsigned long)result);
        return false;
    }
    return true;
#endif
}

static bool qspi_flash_erase_sync(uint32_t addr, size_t length)
{
    cy_rslt_t result;

    if (!qspi_set_xip_enabled(false)) {
        return false;
    }

    result = cy_serial_flash_qspi_erase(addr, length);

    if (!qspi_set_xip_enabled(true)) {
        return false;
    }

    return (result == CY_RSLT_SUCCESS);
}

static bool qspi_flash_write_sync(uint32_t addr, size_t length, const uint8_t *data)
{
    cy_rslt_t result;

    if (!qspi_set_xip_enabled(false)) {
        return false;
    }

    result = cy_serial_flash_qspi_write(addr, length, data);

    if (!qspi_set_xip_enabled(true)) {
        return false;
    }

    return (result == CY_RSLT_SUCCESS);
}

// Simple software CRC32 polynomial 0xEDB88320
static uint32_t crc32_compute(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (-(crc & 1)));
        }
    }
    return ~crc;
}

/*******************************************************************************
 * METADATA MANAGEMENT
 ******************************************************************************/

static bool metadata_append(void)
{
    // Update CRC
    s_meta_cache.crc32 = crc32_compute((const uint8_t*)&s_meta_cache, sizeof(qspi_meta_entry_t) - 4);

    uint32_t entries_per_sector = QSPI_BUF_SECTOR_SIZE / sizeof(qspi_meta_entry_t);
    
    // Check if we need to ping-pong to the other journal sector
    if (s_meta_index >= entries_per_sector) {
        uint32_t next_sector = (s_meta_active_sector == 0) ? 1 : 0;
        uint32_t new_sec_offset = QSPI_BUF_REGION_START + (next_sector * QSPI_BUF_SECTOR_SIZE);
        
        printf("[QSPI_BUF] Journal full, ping-ponging to Sector %lu...\n", (unsigned long)next_sector);
        if (!qspi_flash_erase_sync(new_sec_offset, QSPI_BUF_SECTOR_SIZE)) {
            printf("[QSPI_BUF] WARNING: Failed to erase new journal sector!\n");
            return false;
        }
        
        // Write the entry to the new sector first
        if (!qspi_flash_write_sync(new_sec_offset, sizeof(qspi_meta_entry_t), (const uint8_t*)&s_meta_cache)) {
            printf("[QSPI_BUF] WARNING: Ping-pong metadata write failed!\n");
            return false;
        }
        
        // Transaction complete, update RAM state
        s_meta_active_sector = next_sector;
        s_meta_index = 1;
        return true;
    }

    uint32_t active_base = QSPI_BUF_REGION_START + (s_meta_active_sector * QSPI_BUF_SECTOR_SIZE);
    uint32_t write_addr = active_base + (s_meta_index * sizeof(qspi_meta_entry_t));
    
    // Write the entry
    if (!qspi_flash_write_sync(write_addr, sizeof(qspi_meta_entry_t), (const uint8_t*)&s_meta_cache)) {
        printf("[QSPI_BUF] WARNING: metadata append IO write failed at %lu\n", (unsigned long)write_addr);
        return false;
    }
    
    s_meta_index++;
    return true;
}

static bool metadata_reset(void)
{
    printf("[QSPI_BUF] Re-initializing metadata journal...\n");
    if (!qspi_flash_erase_sync(QSPI_BUF_REGION_START + QSPI_BUF_JOURNAL_0_OFFSET, QSPI_BUF_SECTOR_SIZE)) return false;
    if (!qspi_flash_erase_sync(QSPI_BUF_REGION_START + QSPI_BUF_JOURNAL_1_OFFSET, QSPI_BUF_SECTOR_SIZE)) return false;
    
    // Erase the first data sector as well to ensure clean start
    if (!qspi_flash_erase_sync(QSPI_BUF_REGION_START + QSPI_BUF_DATA_START, QSPI_BUF_SECTOR_SIZE)) return false;

    s_meta_cache.magic = QSPI_META_MAGIC;
    s_meta_cache.seq = 1;
    s_meta_cache.head_offset = QSPI_BUF_DATA_START;
    s_meta_cache.tail_offset = QSPI_BUF_DATA_START;
    s_meta_cache.count = 0;
    s_meta_cache.total_writes = 0;
    
    s_meta_active_sector = 0;
    s_meta_index = 0;
    
    return metadata_append();
}

static bool metadata_recover(void)
{
    printf("[QSPI_BUF] Scanning metadata journal sectors...\n");
    uint32_t entries_per_sector = QSPI_BUF_SECTOR_SIZE / sizeof(qspi_meta_entry_t);
    
    qspi_meta_entry_t best_entry;
    uint32_t best_index = 0;
    uint32_t best_sector = 0;
    uint32_t max_seq = 0;
    bool found_valid = false;

    // Scan Sector 0 and Sector 1
    for (uint32_t sec = 0; sec <= 1; sec++) {
        uint32_t mapped_base = QSPI_MEM_MAPPED_BASE + QSPI_BUF_REGION_START + (sec * QSPI_BUF_SECTOR_SIZE);
        const uint8_t* mapped_journal = (const uint8_t*)mapped_base;

        for (uint32_t i = 0; i < entries_per_sector; i++) {
            qspi_meta_entry_t entry;
            memcpy(&entry, mapped_journal + (i * sizeof(qspi_meta_entry_t)), sizeof(qspi_meta_entry_t));

            if (entry.magic == 0xFFFFFFFFu) break; // Blank flash
            
            if (entry.magic == QSPI_META_MAGIC) {
                uint32_t computed_crc = crc32_compute((const uint8_t*)&entry, sizeof(qspi_meta_entry_t) - 4);
                if (computed_crc == entry.crc32) {
                    if (entry.seq > max_seq) {
                        // Clamp loaded pointers just in case! (Issue #2 FIX)
                        uint32_t max_possible_records = (QSPI_BUF_REGION_SIZE) / 12; // sanity max limits
                        if (entry.head_offset >= QSPI_BUF_DATA_START && entry.head_offset < QSPI_BUF_REGION_SIZE &&
                            entry.tail_offset >= QSPI_BUF_DATA_START && entry.tail_offset < QSPI_BUF_REGION_SIZE &&
                            entry.count <= max_possible_records) {
                            
                            max_seq = entry.seq;
                            best_entry = entry;
                            best_index = i;
                            best_sector = sec;
                            found_valid = true;
                        }
                    }
                }
            }
        }
    }

    if (found_valid) {
        s_meta_cache = best_entry;
        s_meta_active_sector = best_sector;
        s_meta_index = best_index + 1; // Next free slot
        printf("[QSPI_BUF] Recovered seq=%lu, count=%lu (Active Sec=%lu)\n", 
            (unsigned long)s_meta_cache.seq, (unsigned long)s_meta_cache.count, (unsigned long)best_sector);
        return true;
    } else {
        printf("[QSPI_BUF] No valid/sane metadata found.\n");
        return metadata_reset();
    }
}

/*******************************************************************************
 * PUBLIC API
 ******************************************************************************/

bool qspi_buffer_init(void)
{
#ifndef QSPI_BUF_HOST_TEST
    if (s_qspi_mutex == NULL) {
        s_qspi_mutex = xSemaphoreCreateMutex();
        if (s_qspi_mutex == NULL) {
            printf("[QSPI_BUF] ERROR: Failed to create mutex\n");
            return false;
        }
    }
#endif

    if (!qspi_set_xip_enabled(true)) {
        return false;
    }

    if (metadata_recover()) {
        s_is_initialized = true;
        return true;
    }
    return false;
}

static bool room_available(uint32_t h, uint32_t t, uint32_t req)
{
    if (s_meta_cache.count == 0) return true;
    
    uint32_t h_sec = h / QSPI_BUF_SECTOR_SIZE;
    uint32_t t_sec = t / QSPI_BUF_SECTOR_SIZE;

    // Check if head will cross sector boundary
    if ((h % QSPI_BUF_SECTOR_SIZE) + req > QSPI_BUF_SECTOR_SIZE) {
        // Will wrap to next sector
        h = (h_sec + 1) * QSPI_BUF_SECTOR_SIZE;
        if (h >= QSPI_BUF_REGION_SIZE) h = QSPI_BUF_DATA_START;
        
        h_sec = h / QSPI_BUF_SECTOR_SIZE;
        
        // We will erase h_sec! If tail is in it, room is not available.
        if (h_sec == t_sec) return false;
    }
    
    // If they share the same sector, check for direct byte overlap
    if (h_sec == t_sec) {
        if (h < t && (h + req > t)) return false;
    }
    
    return true;
}

bool qspi_buffer_put_record(const buffer_record_t *rec)
{
    if (!s_is_initialized) return false;
    if (rec == NULL || rec->topic[0] == '\0' || rec->payload_len == 0u) return false;

    QSPI_LOCK();

    uint32_t topic_len = strlen(rec->topic);
    uint16_t payload_len = rec->payload_len;
    
    // Truncate sizes so they are naturally valid during peek() and read_record_header()
    if (topic_len > BUFFER_TOPIC_MAX - 1) topic_len = BUFFER_TOPIC_MAX - 1;
    if (payload_len > BUFFER_PAYLOAD_MAX - 1) payload_len = BUFFER_PAYLOAD_MAX - 1;

    uint32_t req_size = sizeof(qspi_data_header_t) + topic_len + payload_len + 4; // +4 for CRC32

    // Ensure space (Consume drops if necessary)
    while (!room_available(s_meta_cache.head_offset, s_meta_cache.tail_offset, req_size)) {
        if (!qspi_buffer_consume_internal(true)) {
            printf("[QSPI_BUF] Error: Failed to drop oldest record!\n");
            QSPI_UNLOCK();
            return false;
        }
    }

    // Determine target location and sector erasures
    uint32_t h = s_meta_cache.head_offset;
    uint32_t h_sec = h / QSPI_BUF_SECTOR_SIZE;
    
    if ((h % QSPI_BUF_SECTOR_SIZE) + req_size > QSPI_BUF_SECTOR_SIZE) {
        // Wrap to next sector
        h = (h_sec + 1) * QSPI_BUF_SECTOR_SIZE;
        if (h >= QSPI_BUF_REGION_SIZE) h = QSPI_BUF_DATA_START;
        
        // Erase the new sector before writing
        if (!qspi_flash_erase_sync(h, QSPI_BUF_SECTOR_SIZE)) {
            QSPI_UNLOCK();
            return false;
        }
    }

    // Prepare buffer to write memory efficiently
    uint8_t write_buf[1024]; // Safe bound since max topic/payload < 600
    if (req_size > sizeof(write_buf)) {
        QSPI_UNLOCK();
        return false;
    }

    qspi_data_header_t hdr;
    hdr.magic = QSPI_RECORD_MAGIC;
    hdr.payload_len = payload_len;
    hdr.topic_len = topic_len;
    hdr.reserved = 0;
    hdr.origin_read_start_ms = rec->origin_read_start_ms;
    hdr.origin_boot_gen = rec->origin_boot_gen;

    memcpy(write_buf, &hdr, sizeof(hdr));
    memcpy(write_buf + sizeof(hdr), rec->topic, topic_len);
    memcpy(write_buf + sizeof(hdr) + topic_len, rec->payload, payload_len);

    uint32_t record_crc = crc32_compute(write_buf, sizeof(hdr) + topic_len + payload_len);
    memcpy(write_buf + sizeof(hdr) + topic_len + payload_len, &record_crc, 4);

    if (!qspi_flash_write_sync(h, req_size, write_buf)) {
        printf("[QSPI_BUF] Write failed at offset %lu\n", (unsigned long)h);
        QSPI_UNLOCK();
        return false;
    }

    // Update metadata and enforce strict boundary wrap logic
    s_meta_cache.head_offset = h + req_size;
    if (s_meta_cache.head_offset >= QSPI_BUF_REGION_SIZE) {
        s_meta_cache.head_offset = QSPI_BUF_DATA_START;
    }
    
    s_meta_cache.count++;
    s_meta_cache.seq++;
    s_meta_cache.total_writes++;
    
    if (!metadata_append()) {
        printf("[QSPI_BUF] WARNING: put metadata append failed\n");
    }
    
    QSPI_UNLOCK();
    metrics_inc_buffer_enqueued();
    return true;
}

bool qspi_buffer_put(const char *topic, const char *payload, uint16_t payload_len)
{
    if (topic == NULL || payload == NULL || payload_len == 0u)
    {
        return false;
    }

    buffer_record_t rec;
    memset(&rec, 0, sizeof(rec));

    strncpy(rec.topic, topic, BUFFER_TOPIC_MAX - 1u);
    rec.topic[BUFFER_TOPIC_MAX - 1u] = '\0';

    uint16_t copy_len = payload_len;
    if (copy_len > BUFFER_PAYLOAD_MAX - 1u) copy_len = BUFFER_PAYLOAD_MAX - 1u;
    memcpy(rec.payload, payload, copy_len);
    rec.payload[copy_len] = '\0';
    rec.payload_len = copy_len;

    return qspi_buffer_put_record(&rec);
}

// Helper safely reads and validates next record header and boundary wraps
static bool read_record_header(uint32_t *offset, qspi_data_header_t *hdr)
{
    uint32_t t = *offset;
    if (t < QSPI_BUF_DATA_START || t >= QSPI_BUF_REGION_SIZE) return false;

    uint32_t magic;
    memcpy(&magic, (const void*)(QSPI_MEM_MAPPED_BASE + t), 4);

    if (magic == 0xFFFFFFFFu) {
        // Padded boundary - skip to next sector
        uint32_t t_sec = t / QSPI_BUF_SECTOR_SIZE;
        t = (t_sec + 1) * QSPI_BUF_SECTOR_SIZE;
        if (t >= QSPI_BUF_REGION_SIZE) t = QSPI_BUF_DATA_START;
        memcpy(&magic, (const void*)(QSPI_MEM_MAPPED_BASE + t), 4);
    }

    if (magic != QSPI_RECORD_MAGIC) {
        return false; 
    }
    
    memcpy(hdr, (const void*)(QSPI_MEM_MAPPED_BASE + t), sizeof(qspi_data_header_t));

    // Bounds check on fields to prevent massive/malicious reads (Issue #1 FIX)
    if (hdr->topic_len >= BUFFER_TOPIC_MAX || hdr->payload_len >= BUFFER_PAYLOAD_MAX) {
        return false;
    }
    
    uint32_t req_size = sizeof(qspi_data_header_t) + hdr->topic_len + hdr->payload_len + 4;
    // Check if the record physically exceeds sector
    if ((t % QSPI_BUF_SECTOR_SIZE) + req_size > QSPI_BUF_SECTOR_SIZE) {
        return false;
    }

    *offset = t; // Update caller's offset to the actual start if we wrapped
    return true;
}

bool qspi_buffer_peek(buffer_record_t *out)
{
    if (!s_is_initialized || s_meta_cache.count == 0 || out == NULL) return false;

    QSPI_LOCK();

    uint32_t t = s_meta_cache.tail_offset;
    qspi_data_header_t hdr;
    
    if (!read_record_header(&t, &hdr)) {
        printf("[QSPI_BUF] Peek corruption (invalid magic or OOB fields) at %lu\n", (unsigned long)t);
        qspi_buffer_consume_internal(true); // Drop corrupted record
        QSPI_UNLOCK();
        return false;
    }

    uint32_t mapped_addr = QSPI_MEM_MAPPED_BASE + t;
    uint32_t req_size = sizeof(qspi_data_header_t) + hdr.topic_len + hdr.payload_len + 4;

    // Validate CRC
    uint32_t computed_crc = crc32_compute((const uint8_t*)mapped_addr, req_size - 4);
    uint32_t stored_crc;
    memcpy(&stored_crc, (const void*)(mapped_addr + req_size - 4), 4);

    if (computed_crc != stored_crc) {
        printf("[QSPI_BUF] CRC mismatch at offset %lu\n", (unsigned long)t);
        qspi_buffer_consume_internal(true); // Drop corrupted record
        QSPI_UNLOCK();
        return false;
    }
    
    // Extract
    memcpy(out->topic, (const void*)(mapped_addr + sizeof(qspi_data_header_t)), hdr.topic_len);
    out->topic[hdr.topic_len] = '\0';

    memcpy(out->payload, (const void*)(mapped_addr + sizeof(qspi_data_header_t) + hdr.topic_len), hdr.payload_len);
    out->payload[hdr.payload_len] = '\0';
    out->payload_len = hdr.payload_len;
    out->origin_read_start_ms = hdr.origin_read_start_ms;
    out->origin_boot_gen = hdr.origin_boot_gen;
    
    QSPI_UNLOCK();
    return true;
}

static bool qspi_buffer_consume_internal(bool is_drop)
{
    if (!s_is_initialized || s_meta_cache.count == 0) return false;

    uint32_t t = s_meta_cache.tail_offset;
    qspi_data_header_t hdr;
    
    if (!read_record_header(&t, &hdr)) {
        // Hard unrecoverable tail, skip aggressively to the next sector boundary
        uint32_t t_sec = t / QSPI_BUF_SECTOR_SIZE;
        s_meta_cache.tail_offset = (t_sec + 1) * QSPI_BUF_SECTOR_SIZE;
    } else {
        uint32_t req_size = sizeof(qspi_data_header_t) + hdr.topic_len + hdr.payload_len + 4;
        s_meta_cache.tail_offset = t + req_size;
    }

    if (s_meta_cache.tail_offset >= QSPI_BUF_REGION_SIZE) {
        s_meta_cache.tail_offset = QSPI_BUF_DATA_START;
    }

    s_meta_cache.count--;
    s_meta_cache.seq++;
    
    if (!metadata_append()) {
        printf("[QSPI_BUF] WARNING: consume metadata update failed\n");
    }

    if (is_drop) {
        metrics_inc_buffer_dropped();
    } else {
        metrics_inc_buffer_dequeued();
    }
    return true;
}

bool qspi_buffer_consume(void)
{
    QSPI_LOCK();
    bool result = qspi_buffer_consume_internal(false);
    QSPI_UNLOCK();
    return result;
}

uint32_t qspi_buffer_depth(void)
{
    if (!s_is_initialized) return 0;
    return s_meta_cache.count;
}

uint32_t qspi_buffer_total_writes(void)
{
    if (!s_is_initialized) return 0;
    return s_meta_cache.total_writes;
}

bool qspi_buffer_erase_all(void)
{
    printf("[QSPI_BUF] Erasing completely...\n");
    for (uint32_t i = 2; i < QSPI_BUF_TOTAL_SECTORS; i++) { // Start at 2 to spare 0/1 journal
        uint32_t offset = QSPI_BUF_REGION_START + (i * QSPI_BUF_SECTOR_SIZE);
        if (!qspi_flash_erase_sync(offset, QSPI_BUF_SECTOR_SIZE)) {
            return false;
        }
    }
    // metadata_reset erases Sector 0 and 1, taking care of journal
    return metadata_reset();
}
