# Persistent Buffer (Store-and-Forward) — MVP Design

This document defines the persistent store-and-forward buffer used by the gateway when MQTT is offline.

## 1) Goals (MVP)
- Do not lose telemetry/status messages during Wi-Fi/MQTT outages (until storage capacity is exceeded).
- Survive gateway reboot/power loss.
- Deterministic behavior on overflow (`drop_oldest`).
- Fast recovery and simple implementation (append-only log).

Non-goals:
- Local database, complex queries, compaction.

---

## 2) High-level design
- Data records are appended to a log region: **append-only**.
- A separate metadata region stores checkpoints for tail/head pointers in an **append-only meta ring** (or FRAM if available).
- On boot, the gateway recovers pointers from the last valid checkpoint and validates records via CRC.

---

## 3) Record format (binary)

All integers are little-endian.

### 3.1 Header (fixed size)
| Field       | Size | Notes |
|------------|------|-------|
| magic      | 2    | 0xB1 0xF0 (example) |
| version    | 1    | 0x01 |
| type       | 1    | 0=telemetry, 1=status, 2=event (optional) |
| len        | 2    | payload length in bytes |
| seq        | 4    | message sequence |
| ts_ms      | 8    | monotonic or unix ms; must be consistent with MQTT payload |
| crc32      | 4    | CRC32 of payload only |
Total header: **22 bytes**

### 3.2 Payload
Payload is either:
- **Option A (recommended):** UTF-8 JSON bytes matching MQTT schema (easy, but bigger)
- **Option B:** compact struct; JSON is built on flush (smaller, more code)

MVP recommendation: start with **Option A** to reduce complexity and ensure contract consistency.

### 3.3 Record layout
`[header][payload bytes (len)]`

A record is valid if:
- `magic` matches
- `version` supported
- `len` within bounds
- `crc32(payload)` matches

---

## 4) Regions and pointers

### 4.1 Log region
A ring buffer over a fixed storage partition:
- `log_start`, `log_end` (exclusive)
- pointers:
  - `tail` = offset of next record to **read/flush**
  - `head` = offset of next record to **write**

When `head` reaches `log_end`, wrap to `log_start`.

### 4.2 Metadata region (no-FRAM MVP)
To avoid flash wear from rewriting one sector, store checkpoints append-only in a meta ring:

Checkpoint record:
- `magic_meta` (2B)
- `version` (1B)
- `tail_off` (4B)
- `head_off` (4B)
- `tail_seq` (4B)
- `head_seq` (4B)
- `crc32` (4B) over previous fields

Write a checkpoint every **N appended records** (recommended N=64) OR every **T seconds** (e.g., 5s), whichever comes first.

### 4.3 If FRAM is available
Store `tail_off`, `head_off`, `tail_seq`, `head_seq` directly in FRAM with a simple double-buffer + CRC.

---

## 5) Overflow policy (`drop_oldest`)

When there is not enough contiguous free space to append a new record:
1) Advance `tail` (discard oldest record) until enough space exists.
2) Increment `buffer_dropped`.
3) Emit event `BUFFER_OVERFLOW` with detail including dropped count (optional).

Discarding a record:
- Validate header minimally; if header invalid, move forward by 1 byte until a valid header is found (resync).
- Otherwise, `tail += header_size + len` (with wrap).

---

## 6) Boot recovery procedure

1) Load last valid checkpoint from meta ring (scan from newest backwards until CRC ok).
2) Set `tail`, `head`, `tail_seq`, `head_seq` from checkpoint.
3) Validate the record at `tail` (if invalid, resync forward).
4) Validate that `head` position is writable (optional).
5) Reconstruct gateway global `seq`:
   - `seq = max(head_seq, last_valid_record_seq) + 1`
   - if scanning is expensive, trust `head_seq` from checkpoint.

---

## 7) Flush procedure

When MQTT is online:
- Dequeue up to `flush_batch_size` records per tick or per `flush_interval_ms`.
- For each record:
  - parse header, validate CRC
  - publish payload to the correct topic based on `type` and stored topic parameters
  - on publish success: advance `tail` and increment `buffer_dequeued`
  - on publish failure: stop flush and retry later (do not drop)

After flushing a batch:
- write a checkpoint (optional immediate) or rely on periodic checkpointing.

---

## 8) Implementation interfaces (suggested)

```c
bool buffer_init(void);
bool buffer_put(uint8_t type, uint32_t seq, uint64_t ts_ms, const uint8_t* payload, uint16_t len);
bool buffer_peek(record_view_t* out);      // view next record without consuming
bool buffer_consume(void);                // advance tail after successful publish
uint32_t buffer_depth_estimate(void);      // approximate count or bytes
```

---

## 9) Notes for experiments
- Exp3 requires persistent buffering to survive a reboot mid-outage.
- Include buffer depth gauges (`buffer_depth_ram`, `buffer_depth_flash`) and counters (`buffer_enqueued/dequeued/dropped`) in metrics.
