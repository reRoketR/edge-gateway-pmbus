# Persistent Buffer (Store-and-Forward) — Design & Implementation

This document describes the two-tier offline store-and-forward buffer used by
the PMBus→MQTT edge gateway.

## 1) Goals
- Do not lose telemetry/status messages during Wi-Fi/MQTT outages (until storage capacity is exceeded).
- Survive gateway reboot/power loss (flash tier).
- Deterministic behavior on overflow (`drop_oldest`).
- Simple implementation: one 512-byte flash row per record.
- Two-tier architecture: fast RAM ring buffer + persistent flash tier.

Non-goals:
- Local database, complex queries, compaction.

## Status

**Implemented** in:
- `source/buffer_mgr.c` / `source/buffer_mgr.h` — RAM ring buffer tier
- `source/flash_buffer.c` / `source/flash_buffer.h` — Flash-backed persistent tier

---

## 2) High-level design

```
  Task A (poll) → IPC queue → Task B (mqtt_gw) --offline?-→ buffer_mgr_put()
                                                                 |
                                          ┌──────────────────────┤
                                          ▼                      ▼ (overflow)
                                     RAM ring buf          flash_buffer_put()
                                          ▲                      ▲
                                          └──────────────────────┘
  Task B: flush_buffered_records() ← flash_peek/consume, ram_peek/consume
```

- When MQTT is **offline**, `mqtt_gw_task` stores unsent records via
  `buffer_mgr_put()` into the RAM ring buffer.
- When the RAM tier is full, records spill to the flash tier via
  `flash_buffer_put()`.
- When MQTT is **online**, `flush_buffered_records()` in `mqtt_gw_task`
  drains flash first (FIFO), then RAM (FIFO), publishing each record via
  the single MQTT publisher.

---

## 3) RAM tier — `buffer_mgr.c`

A fixed-size ring buffer of `buffer_record_t` structs.

```c
typedef struct {
    char     topic[80];
    char     payload[512];
    uint16_t payload_len;
} buffer_record_t;
```

- Capacity: `g_config.buffer.ram_records` (typically 32–128).
- Operations: `buffer_mgr_put()`, `buffer_mgr_peek()`, `buffer_mgr_consume()`,
  `buffer_mgr_get()` (legacy dequeue).
- Protected by `taskENTER_CRITICAL()` / `taskEXIT_CRITICAL()` (ISR-safe).
- No persistence across reboot.

---

## 4) Flash tier — `flash_buffer.c`

Uses the PSoC 6 **Em_EEPROM** flash region (32 KB at `0x14000000`).

### 4.1 Physical layout

| Row(s)    | Size         | Purpose                         |
|-----------|-------------|----------------------------------|
| Row 0     | 512 B        | Metadata (head/tail/count/CRC)  |
| Rows 1–63 | 512 B each  | Data records (one per row)      |

Total capacity: **63 records** (or fewer if `flash_max_records` is configured
lower).

### 4.2 Data row format (`flash_data_row_t`, 512 bytes)

| Offset | Field        | Size    | Notes                                |
|--------|-------------|---------|--------------------------------------|
| 0      | magic       | 4 B     | `0xB1F0DA7A`                         |
| 4      | payload_len | 2 B     | Actual payload length                |
| 6      | reserved    | 2 B     | Padding (0x0000)                     |
| 8      | topic       | 80 B    | MQTT topic, NUL-terminated           |
| 88     | payload     | 420 B   | JSON payload, NUL-terminated         |
| 508    | crc32       | 4 B     | CRC-32 of bytes 0–507                |

All integers are **little-endian** (ARM Cortex-M4 native).

A record is valid when:
1. `magic == 0xB1F0DA7A`
2. `payload_len <= 420`
3. `crc32` matches computed CRC-32 of bytes 0–507

### 4.3 Metadata row format (`flash_meta_row_t`, 512 bytes)

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

### 4.5 Flash endurance

With 63 data rows and round-robin writes, effective endurance is
~6.3 million records before wear-out (100 K cycles per row × 63 rows).

---

## 5) Overflow policy

When both tiers are full and `drop_oldest == true`:
1. Flash tier advances tail (discards oldest flash record).
2. RAM tier drops head record on put.
3. `buffer_dropped` metric is incremented for each dropped record.

When `drop_oldest == false`, new records are silently discarded.

---

## 6) Boot recovery

1. `flash_buffer_init()` reads the metadata row from flash.
2. If magic + version + CRC are valid, restores `head`, `tail`, `count`.
3. Head/tail/count are **clamped** to the configured capacity — prevents
   out-of-bounds access if `flash_max_records` was reduced between builds.
4. If metadata is invalid (first boot or corruption), initialises fresh
   (head=0, tail=0, count=0) and writes the metadata row.
5. RAM tier starts empty on every boot (not persisted).

---

## 7) Flush procedure (`flush_buffered_records()`)

Called from `mqtt_gw_task` after draining the IPC queues, while MQTT is online:

1. **Flash tier first** (oldest data):
   - `flash_buffer_peek()` reads the tail record without consuming.
   - If the record is **invalid** (CRC mismatch), the tail is auto-advanced
     to prevent infinite retry loops.
   - On successful MQTT publish: `flash_buffer_consume()` advances the tail.
   - On publish failure: stop flushing (retry next cycle).
   - Up to `flash_max_records` records per flush cycle.

2. **RAM tier second**:
   - `buffer_mgr_peek()` reads the oldest record without consuming.
   - On successful MQTT publish: `buffer_mgr_consume()` removes it.
   - On publish failure: stop flushing.
   - Up to `ram_records` records per flush cycle.

This ensures strict FIFO ordering: older flash records are published before
newer RAM records.

---

## 8) Public API summary

```c
/* RAM tier (buffer_mgr.h) */
void buffer_mgr_init(void);
bool buffer_mgr_put(const char *topic, const char *payload, uint16_t len);
bool buffer_mgr_peek(buffer_record_t *out);
bool buffer_mgr_consume(void);
bool buffer_mgr_get(buffer_record_t *out);  /* legacy: dequeue */
uint32_t buffer_mgr_depth(void);

/* Flash tier (flash_buffer.h) */
bool     flash_buffer_init(void);
bool     flash_buffer_put(const char *topic, const char *payload, uint16_t len);
bool     flash_buffer_peek(buffer_record_t *out);
bool     flash_buffer_consume(void);
uint32_t flash_buffer_depth(void);
uint32_t flash_buffer_total_writes(void);
bool     flash_buffer_erase_all(void);
```

---

## 9) Metrics integration

Exposed via MQTT metrics topic:
- **Gauges**: `buffer_depth_ram`, `buffer_depth_flash`
- **Counters**: `buffer_enqueued`, `buffer_dequeued`, `buffer_dropped`
- **Flash wear**: `total_writes` (reported in flash tier, not yet exposed via
  MQTT but available via `flash_buffer_total_writes()`).

---

## 10) Notes for experiments

- **Exp3** requires persistent buffering to survive a reboot mid-outage.
  Verify that `flash_buffer_depth()` reports non-zero after power-cycle.
- Include buffer depth gauges and counters in the Grafana dashboard.
