# Persistent Buffer (Store-and-Forward) — Design & Implementation

This document describes the two-tier offline store-and-forward buffer used by
the PMBus→MQTT edge gateway.

## 1) Goals
- Do not lose telemetry/status messages during Wi-Fi/MQTT outages (until storage capacity is exceeded).
- Survive gateway reboot/power loss (flash tier).
- Deterministic behavior on overflow (`drop_oldest`).
- Two-tier architecture: fast RAM ring buffer + persistent flash tier.
- Support two persistent backends (Em_EEPROM and QSPI), selected at compile time.

Non-goals:
- Local database, complex queries, compaction.

## Status

**Implemented** in:
- `source/buffer_mgr.c` / `source/buffer_mgr.h` — RAM ring buffer tier
- `source/flash_buffer.c` / `source/flash_buffer.h` — Em_EEPROM persistent tier
- `source/qspi_buffer.c` / `source/qspi_buffer.h` — QSPI NOR flash persistent tier
- `source/persistent_buffer.h` — Compile-time abstraction (wraps one of the above)

---

## 2) High-level design

```
  Task A (poll) ─► IPC queues / emergency ring
                         │
                         ▼
  Task C (buffer_task) ──► buffer_mgr_put_internal()
                         │
             ┌───────────┴───────────┐
             ▼                       ▼ (RAM full → spill)
        RAM ring buf          persistent_buffer_put()
             ▲                       ▲
             └───────────────────────┘
  Task B (mqtt_gw): flush_buffered_records() ◄─ persistent_peek/consume, ram_peek/consume
```

- **Task C** (`buffer_task`) runs continuously and **always** drains all
  FreeRTOS IPC queues (telemetry, status, events) and the emergency ring,
  regardless of MQTT connection state. Each record is JSON-encoded and stored
  via `buffer_mgr_put_internal()`. This task is the sole writer to `buffer_mgr`.
- When the RAM tier is full, a record spills to the persistent tier via
  `persistent_buffer_put()`.
- **Task B** (`mqtt_gw_task`) flushes when MQTT is **online**: drains the
  persistent tier first (oldest data), then the RAM tier, publishing each
  record via the single MQTT publisher. On publish failure it stops and
  retries next cycle.

---

## 3) RAM tier — `buffer_mgr.c`

A fixed-size ring buffer of `buffer_record_t` structs.

```c
typedef struct {
    char     topic[80];
    char     payload[512];
    uint16_t payload_len;
    uint32_t origin_read_start_ms;
    uint32_t origin_boot_gen;
} buffer_record_t;
```

- Capacity: `g_config.buffer.ram_records` (typically 32–128).
- Operations: `buffer_mgr_put()`, `buffer_mgr_peek()`, `buffer_mgr_consume()`.
- Protected by `taskENTER_CRITICAL()` / `taskEXIT_CRITICAL()` (ISR-safe).
- No persistence across reboot.

---

## 4) Em_EEPROM Flash tier — `flash_buffer.c`

Uses the PSoC 6 **Em_EEPROM** flash region (32 KB at `0x14000000`).

### 4.1 Physical layout

| Row(s)     | Size        | Purpose                                       |
|------------|------------|-----------------------------------------------|
| Row 0      | 512 B       | Metadata (head/tail/count/total_writes/CRC)   |
| Rows 1–61  | 512 B each  | Data records (one per row, max 61 records)    |
| Rows 62–63 | 512 B each  | Reserved for `persistent_seq` A/B banks       |

Total data capacity: **61 records** (or fewer if `flash_max_records` is
configured lower).

### 4.2 Data row format (512 bytes)

| Offset | Field                  | Size  | Notes                              |
|--------|------------------------|-------|------------------------------------|
| 0      | magic                  | 4 B   | `0xB1F0DA7A`                       |
| 4      | payload_len            | 2 B   | Actual payload length              |
| 6      | reserved               | 2 B   | Padding (0x0000)                   |
| 8      | origin_read_start_ms   | 4 B   | PMBus read-start monotonic ms      |
| 12     | origin_boot_gen        | 4 B   | Boot generation of origin          |
| 16     | topic                  | 80 B  | MQTT topic, NUL-terminated         |
| 96     | payload                | 412 B | JSON payload, NUL-terminated       |
| 508    | crc32                  | 4 B   | CRC-32 of bytes 0–507              |

All integers are **little-endian** (ARM Cortex-M4 native).

A record is valid when:
1. `magic == 0xB1F0DA7A`
2. `payload_len <= 412`
3. `crc32` matches computed CRC-32 of bytes 0–507

### 4.3 Metadata row format (512 bytes)

| Offset | Field        | Size   | Notes                                |
|--------|-------------|--------|--------------------------------------|
| 0      | magic       | 4 B    | `0x4D455441` ("META")                |
| 4      | head        | 2 B    | Next write slot (0..capacity-1)      |
| 6      | tail        | 2 B    | Next read slot (0..capacity-1)       |
| 8      | count       | 2 B    | Number of valid records              |
| 10     | version     | 2 B    | Format version (1)                   |
| 12     | total_writes| 4 B    | Cumulative row writes (wear metric)  |
| 16     | crc32       | 4 B    | CRC-32 of bytes 0–15                 |
| 20     | _pad        | 492 B  | `0xFF` (erased state)                |

### 4.4 Flash API

- `Cy_Flash_WriteRow()` — pre-program + erase + write, blocks ~16 ms.
- `Cy_Flash_EraseRow()` — erase a single row.
- Direct pointer reads (flash is memory-mapped).

Each `flash_buffer_put()` performs **two** flash writes: one data row + one
metadata row.  `flash_buffer_consume()` performs one metadata write.

### 4.5 Em_EEPROM endurance

With 61 data rows and round-robin writes, effective endurance is
≈6.1 million records before wear-out (100 K erase cycles per row × 61 rows).

---

## 5) QSPI Flash tier — `qspi_buffer.c`

Uses the external **S25FL512S** NOR flash via the QSPI interface.  The buffer
occupies a dedicated 2 MB region beginning at QSPI address `0x00000000`
(memory-mapped to `0x18000000` in the PSoC 6 address space).

### 5.1 Physical layout

| Sector(s) | Size         | Purpose                                    |
|-----------|-------------|---------------------------------------------|
| 0–1       | 256 KB each | Metadata journal (ping-pong, wear-leveled)  |
| 2–7       | 256 KB each | Data ring buffer (6 sectors = 1.5 MB)       |

The two metadata sectors use a journaling strategy: new entries are appended
sequentially within the active sector until it is full, then the other sector
is erased and becomes active.  This limits sector-erase frequency for the
metadata region.

### 5.2 Metadata journal entry format (`qspi_meta_entry_t`, 28 bytes)

Capacity per sector: **9,362 entries** (⌊262 144 ÷ 28⌋).

| Offset | Field        | Size  | Notes                                    |
|--------|-------------|-------|------------------------------------------|
| 0      | magic       | 4 B   | `0x4D455432` ("MET2")                    |
| 4      | seq         | 4 B   | Monotonically increasing sequence number |
| 8      | head_offset | 4 B   | Absolute QSPI offset for next write      |
| 12     | tail_offset | 4 B   | Absolute QSPI offset for oldest record   |
| 16     | count       | 4 B   | Number of valid records in the ring      |
| 20     | total_writes| 4 B   | Lifetime record write counter            |
| 24     | crc32       | 4 B   | CRC32 of bytes 0–23                      |

The latest valid entry (highest `seq` with matching CRC) is used on boot
recovery.

### 5.3 Data record format

Each record is written as a variable-length packet:

```
[ qspi_data_header_t (16 B) | topic bytes | payload bytes | crc32 (4 B) ]
```

**`qspi_data_header_t` (16 bytes, packed):**

| Offset | Field                | Size  | Notes                             |
|--------|---------------------|-------|-----------------------------------|
| 0      | magic               | 4 B   | `0x52454344` ("RECD")             |
| 4      | payload_len         | 2 B   | Actual payload length             |
| 6      | topic_len           | 1 B   | Actual topic length               |
| 7      | reserved            | 1 B   | Future use (0x00)                 |
| 8      | origin_read_start_ms| 4 B   | PMBus read-start monotonic ms     |
| 12     | origin_boot_gen     | 4 B   | Boot generation of origin         |

Variable-length record size = 16 + `topic_len` + `payload_len` + 4 bytes CRC.
For a typical 232-byte payload + 44-byte topic: **296 bytes per record**.

### 5.4 QSPI endurance

| Parameter                    | Value                             |
|------------------------------|-----------------------------------|
| Sector erase cycles          | ~100K per sector                  |
| Data sectors                 | 6 (sectors 2–7)                   |
| Data region effective cycles | ~600K sector erases before wearout|
| Journal sector switches      | Every ~9,362 entries per sector   |
| Metadata endurance           | ~18,724 metadata writes per full  |
|                              | journal ping-pong cycle           |

With ~296-byte average records and 1.5 MB data region, the ring holds
approximately **5,300 records** before the oldest is overwritten (much larger
than the Em_EEPROM tier).

### 5.5 Boot recovery

1. `qspi_buffer_init()` scans both journal sectors for the entry with the
   highest valid `seq` + matching CRC32.
2. `head_offset`, `tail_offset`, and `count` are restored from that entry.
3. If no valid entry is found (first boot or corruption), initialises fresh
   (`head = tail = QSPI_BUF_DATA_START`, `count = 0`) and writes a new entry.
4. RAM tier starts empty on every boot (not persisted).

---

## 6) Capacity comparison

| Property                | Em_EEPROM (`flash_buffer`) | QSPI (`qspi_buffer`)      |
|-------------------------|---------------------------|---------------------------|
| Flash chip              | Internal PSoC 6 Em_EEPROM | External S25FL512S NOR    |
| Region size             | 32 KB (2 KB data rows)    | 2 MB                      |
| Data region             | 61 × 512 B rows = 30.5 KB | 6 × 256 KB = 1.5 MB       |
| Max capacity (fixed)    | **61 records**            | ~**5,300 records** (296 B avg) |
| Record size             | Fixed 512 B               | Variable (header + payload)|
| Sector erase endurance  | ~100 K / row              | ~100 K / sector           |
| Effective endurance     | ~6.1 M records            | ~600 K sector erases      |
| Metadata strategy       | Single row, in-place      | Append-only journal, ping-pong |
| Write latency           | ~16 ms (Cy_Flash_WriteRow)| ~1–3 ms (SPI NOR program) |
| Boot-time build flag    | (default)                 | `BUFFER_BACKEND=QSPI`     |

---

## 7) Compile-time backend selection — `persistent_buffer.h`

The `persistent_buffer.h` header provides a uniform `persistent_buffer_*` API
that maps to the selected backend at compile time via preprocessor macros:

```makefile
make build                      # Em_EEPROM backend (default)
make build BUFFER_BACKEND=QSPI  # QSPI NOR flash backend
```

```c
/* In persistent_buffer.h: */
#if defined(BUFFER_BACKEND_QSPI)
  #define persistent_buffer_init         qspi_buffer_init
  #define persistent_buffer_put          qspi_buffer_put
  #define persistent_buffer_peek         qspi_buffer_peek
  #define persistent_buffer_consume      qspi_buffer_consume
  #define persistent_buffer_depth        qspi_buffer_depth
  #define persistent_buffer_total_writes qspi_buffer_total_writes
  #define persistent_buffer_erase_all    qspi_buffer_erase_all
  #define PERSISTENT_BACKEND_NAME        "QSPI (S25FL512S)"
#else
  #define persistent_buffer_init         flash_buffer_init
  /* ... etc. */
  #define PERSISTENT_BACKEND_NAME        "Em_EEPROM (internal)"
#endif
```

`buffer_mgr.c` calls `persistent_buffer_*` exclusively.  No `#ifdef` is
needed in `buffer_mgr.c` except for the `qspi_flash.h` SMIF initialisation
include.

---

## 8) Migration from Em_EEPROM to QSPI

If migrating an existing deployment from the Em_EEPROM backend to QSPI:

1. **Drain and confirm empty**: Ensure all buffered records are flushed to the
   broker while still running the Em_EEPROM build.  Confirm
   `buffer_depth_flash == 0` in the metrics JSON before proceeding.

2. **Erase the QSPI region**: On first use, QSPI NOR flash may contain
   arbitrary data.  Call `qspi_buffer_erase_all()` once (or use the
   `make erase MTB_ERASE_EXT_MEM=1` OpenOCD helper) to wipe sectors 0–7.

3. **Rebuild with the QSPI flag**:
   ```bash
   make build TOOLCHAIN=GCC_ARM CONFIG=Debug BUFFER_BACKEND=QSPI
   make program
   ```

4. **Verify boot banner**: The UART boot banner shows the active backend:
   ```
   [BUF] Persistent backend: QSPI (S25FL512S)
   ```

5. **No data migration is possible**: The two backends use incompatible
   on-flash layouts.  Any records still in Em_EEPROM flash will not be
   readable by the QSPI build.  Drain before switching (see step 1).

6. **Reverting**: Rebuild without the flag to revert to Em_EEPROM.  The QSPI
   region is left intact and will be picked up on the next QSPI build.

---

## 9) Overflow policy

When both tiers are full and `drop_oldest == true`:
1. Flash tier advances tail (discards oldest flash record).
2. RAM tier drops head record on put.
3. `buffer_dropped` metric is incremented for each dropped record.

When `drop_oldest == false`, new records are silently discarded.

---

## 10) Boot recovery

### Em_EEPROM
1. `flash_buffer_init()` reads the metadata row from flash.
2. If magic + version + CRC are valid, restores `head`, `tail`, `count`.
3. Values are clamped to configured capacity (guards against `flash_max_records`
   reduction between builds).
4. If metadata is invalid, initialises fresh and writes the metadata row.

### QSPI
1. `qspi_buffer_init()` scans both journal sectors (see §5.5 above).
2. Uses the entry with the highest valid `seq`.
3. On invalid/empty flash, writes an initial journal entry.

Both backends: RAM tier starts empty on every boot.

---

## 11) Flush procedure (`flush_buffered_records()`)

Called from `mqtt_gw_task` after draining the IPC queues, while MQTT is online:

1. **Persistent tier first** (oldest data):
   - `persistent_buffer_peek()` reads the tail record without consuming.
   - If the record is **invalid** (CRC mismatch), the tail is auto-advanced
     to prevent infinite retry loops.
   - On successful MQTT publish: `persistent_buffer_consume()` advances tail.
   - On publish failure: stop flushing (retry next cycle).

2. **RAM tier second**:
   - `buffer_mgr_peek()` reads the oldest record without consuming.
   - On successful MQTT publish: `buffer_mgr_consume()` removes it.
   - On publish failure: stop flushing.

This ensures strict FIFO ordering: older persistent records are published
before newer RAM records.

---

## 12) Public API summary

```c
/* RAM tier (buffer_mgr.h) */
bool     buffer_mgr_init(void);
bool     buffer_mgr_put(const char *topic, const char *payload, uint16_t len);
bool     buffer_mgr_peek(buffer_record_t *out);
bool     buffer_mgr_consume(void);
uint32_t buffer_mgr_depth(void);

/* Em_EEPROM backend (flash_buffer.h) */
bool     flash_buffer_init(void);
bool     flash_buffer_put(const char *topic, const char *payload, uint16_t len);
bool     flash_buffer_peek(buffer_record_t *out);
bool     flash_buffer_consume(void);
uint32_t flash_buffer_depth(void);
uint32_t flash_buffer_total_writes(void);
bool     flash_buffer_erase_all(void);

/* QSPI backend (qspi_buffer.h) */
bool     qspi_buffer_init(void);
bool     qspi_buffer_put(const char *topic, const char *payload, uint16_t payload_len);
bool     qspi_buffer_peek(buffer_record_t *out);
bool     qspi_buffer_consume(void);
uint32_t qspi_buffer_depth(void);
uint32_t qspi_buffer_total_writes(void);
bool     qspi_buffer_erase_all(void);

/* Abstraction layer (persistent_buffer.h) — maps to selected backend */
#define  persistent_buffer_init         /* → flash_buffer_init or qspi_buffer_init   */
#define  persistent_buffer_put          /* → flash_buffer_put  or qspi_buffer_put    */
#define  persistent_buffer_total_writes /* → flash_buffer_total_writes or qspi_…     */
```

---

## 13) Metrics integration

Exposed via MQTT metrics topic (`<base>/metrics`), in the `"gauges"` object:
- **`buffer_depth_ram`**, **`buffer_depth_flash`** — current record counts
- **Counters**: `buffer_enqueued`, `buffer_dequeued`, `buffer_dropped`
- **`storage`** (nested object, D2b-4):
  ```json
  "storage":{"backend":"qspi","total_writes":1234}
  ```
  - `backend`: string `"qspi"` or `"eeprom"`, set at compile time
  - `total_writes`: lifetime write count from `persistent_buffer_total_writes()`,
    updated each drain cycle by `buffer_mgr_drain_once()`

---

## 14) Notes for experiments

- **Exp3** requires persistent buffering to survive a reboot mid-outage.
  Verify that `persistent_buffer_depth()` reports non-zero after power-cycle.
- Use `BUFFER_BACKEND=QSPI` build to test the QSPI tier (see §7).
- Include `buffer_depth_ram`, `buffer_depth_flash`, `storage_total_writes`,
  and `storage_backend` gauges in the Grafana dashboard for wear monitoring.
