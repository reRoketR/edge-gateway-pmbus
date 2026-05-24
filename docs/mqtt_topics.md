# MQTT Topics & Contracts (MVP)

This document defines the **stable MQTT contract** for the PMBus↔MQTT gateway.

## 1) Topic scheme

Base topic: `pmbus/{gw_id}`

### 1.1 Telemetry (per device)
- Topic: `pmbus/{gw_id}/dev/{addr}/telemetry`
- `{addr}` MUST be a string formatted as `0xNN` (example: `0x58`)

### 1.2 Status (per device)
- Topic: `pmbus/{gw_id}/dev/{addr}/status`

### 1.3 Gateway metrics
- Topic: `pmbus/{gw_id}/metrics`

### 1.4 Gateway events
- Topic: `pmbus/{gw_id}/events`

### 1.5 Remote command request
- Topic: `pmbus/{gw_id}/cmd/request`
- Direction: MQTT client -> gateway
- Purpose: execute one explicit generic SMBus/PMBus transfer.

### 1.6 Remote command response
- Topic: `pmbus/{gw_id}/cmd/response`
- Direction: gateway -> MQTT client
- Purpose: return the result of a command request with the same correlation ID.

---

## 2) QoS / retain policy (MVP)

| Stream    | QoS | retain |
|----------|-----|--------|
| telemetry| 0   | false  |
| status   | 1   | false  |
| events   | 1   | false  |
| metrics  | 0   | false  |
| cmd/request | 1 | false |
| cmd/response | 1 | false |

Notes:
- QoS1 may produce duplicates after reconnects; this mainly applies to `status` and `events`.
- Telemetry uses QoS0 in the current firmware to reduce publish-path latency and tail jitter.
- Command requests/responses use a correlation `id`; the firmware keeps a
  recent-response cache to suppress duplicate QoS1 request execution where
  possible.
- `retain=false` for all streams to avoid stale state surprises.

---

## 3) Deduplication policy (MVP)

Duplicates **are acceptable**. Consumers MUST deduplicate by key:

`(gw_id, addr, seq)`

Rules:
- `seq` is monotonic per gateway instance.
- `seq` is checkpointed best-effort via `persistent_seq`.
- After reboot, the last checkpoint is restored rather than resetting to `0`.
- Up to `PERSISTENT_SEQ_CHECKPOINT_INTERVAL` values can roll back after crash/power loss.
- Strict cross-reboot deduplication is NOT supported.

---

## 4) Payload schemas

All payloads are JSON.

### 4.1 Telemetry payload (single snapshot)

Topic: `.../telemetry`

```json
{
  "ts_ms": 1730000000000,
  "time_synced": true,
  "boot_count": 42,
  "sample_monotonic_ms": 98765,
  "seq": 12345,
  "gw_id": "gw01",
  "addr": "0x58",
  "label": "psu_a",
  "pec": true,
  "read_ms": 7,
  "retries": 0,
  "v": { "vin": 12.03, "vout": 1.02 },
  "a": { "iin": 0.84, "iout": 5.10 },
  "c": { "temp1": 42.5 },
  "w": { "pout": 5.20 },
  "raw": { "read_vout": "0x0123" }
}
```

Field rules:
- `time_synced` — always present. `true` once SNTP has obtained a valid epoch; `false` when `ts_ms` is an uptime-ms fallback.
- `boot_count` — always present. Persistent boot/session identifier restored from `persistent_seq`.
- `sample_monotonic_ms` — always present. Monotonic sample timestamp from the FreeRTOS tick domain; resets on reboot and is intended to be interpreted together with `boot_count`.
- Units are SI base units: V, A, °C, W.
- `read_ms` = PMBus snapshot duration only (first PMBus command start → last PMBus response received). Excludes MQTT publish.
- `retries` = total retries used across all PMBus commands in this snapshot.
- `raw` is optional (recommended in DEBUG builds).

### 4.2 Status payload

Topic: `.../status`

```json
{
  "ts_ms": 1730000000000,
  "time_synced": true,
  "seq": 12345,
  "gw_id": "gw01",
  "addr": "0x58",
  "label": "psu_a",
  "status_word": "0x8040",
  "status_vout": "0x0000",
  "status_iout": "0x0000",
  "status_temperature": "0x0000"
}
```

Field rules:
- `time_synced` — always present (see §4.1).
- `status_*` fields are hex-encoded register values; width matches the underlying PMBus register.

### 4.3 Metrics payload

Topic: `.../metrics`

```json
{
  "ts_ms": 1730000002000,
  "window_ms": 2000,
  "counters_delta": {
    "pmbus_reads_ok": 240,
    "pmbus_reads_fail": 3,
    "pmbus_retries": 12,
    "pmbus_timeouts": 2,
    "pmbus_nack": 1,
    "pmbus_crc_pec_fail": 0,
    "mqtt_pub_ok": 98,
    "mqtt_pub_fail": 4,
    "mqtt_reconnects": 1,
    "buffer_enqueued": 102,
    "buffer_dequeued": 80,
    "buffer_dropped": 0,
    "queue_drops": 0,
    "telemetry_enqueued": 98,
    "i2c_controller_resets": 0,
    "i2c_bus_recoveries": 0
  },
  "gauges": {
    "buffer_depth_ram": 120,
    "buffer_depth_flash": 3500,
    "telemetry_queue_depth": 3,
    "wifi_rssi_dbm": -56,
    "uptime_s": 1843
  },
  "timing_ms": {
    "read_to_publish_avg": 18.2,
    "read_to_publish_p95": 34.0,
    "read_to_publish_max": 61.0,
    "telemetry_before_publish_avg": 14.1,
    "telemetry_publish_avg": 4.1,
    "pmbus_txn_avg": 6.4,
    "pmbus_txn_max": 19.0,
    "mqtt_publish_avg": 4.1,
    "mqtt_publish_max": 20.0
  },
  "timing_rolling_ms": {
    "read_to_publish_avg": 19.8,
    "read_to_publish_p95": 40.0,
    "read_to_publish_max": 120.0
  },
  "timing_samples": {
    "read_to_publish_window": 98,
    "read_to_publish_rolling": 100
  },
  "rates": {
    "telemetry_msgs_per_s": 49.0,
    "pmbus_cmds_per_s": 120.0
  }
}
```

Rules:
- `counters_delta` are increments over `window_ms` and are reset after publish.
- `read_to_publish_*` MUST be measured on the gateway using a monotonic timer.
- `timing_ms.read_to_publish_*` are **per metrics window** and reset after every
  metrics publish. Consumers should treat them as absent when
  `timing_samples.read_to_publish_window == 0`.
- `timing_ms.telemetry_before_publish_avg` and
  `timing_ms.telemetry_publish_avg` are computed from the same telemetry samples
  as `read_to_publish_avg`, so their averages are additive:
  `read_to_publish_avg = telemetry_before_publish_avg + telemetry_publish_avg`.
  `telemetry_before_publish_avg` includes the PMBus read plus any wait before
  the telemetry publish begins; it is intentionally not the same aggregate as
  `pmbus_txn_avg`.
- `timing_rolling_ms.read_to_publish_*` keep the latest `N` samples for
  long-tail diagnosis. The current firmware uses `N=100`.
- Timing percentiles are diagnostic/approximate snapshots, not a hard real-time
  measurement contract.

### 4.4 Events payload

Topic: `.../events`

```json
{
  "ts_ms": 1730000000000,
  "time_synced": true,
  "type": "MQTT_DISCONNECTED",
  "detail": "wifi_lost"
}
```

Event types (MVP):
- `time_synced` — always present (see §4.1).
- `MQTT_CONNECTED`, `MQTT_DISCONNECTED`
- `PMBUS_DEVICE_OFFLINE`, `PMBUS_DEVICE_ONLINE` (include addr in detail or add field)
- `PMBUS_BUS_RECOVERY`, `PMBUS_BUS_RECOVERY_FAILED`
- `I2C_CONTROLLER_RESET` (D1-2: SCB disable/re-enable path on timeout/not-ready)
- `BUFFER_OVERFLOW`, `QUEUE_OVERFLOW`

### 4.5 Remote command request

Topic: `pmbus/{gw_id}/cmd/request`

```json
{
  "id": "r001",
  "addr": 88,
  "wr": [136],
  "rd_len": 2,
  "pec": true
}
```

Field rules:
- `id` is a caller-supplied correlation ID, max `CMD_ID_MAX - 1` characters.
- `addr` is the 7-bit SMBus/I2C address as a number, for example `88` for
  `0x58`.
- `wr` is an optional byte array, max `CMD_MAX_WRITE_LEN` bytes.
- `rd_len` is the requested read length, max `CMD_MAX_READ_LEN` bytes.
- `pec` is optional; when omitted, the request parser uses the default command
  path behavior.

Supported transfer shapes:

- write-only: `wr` present, `rd_len = 0`;
- read-only: no `wr`, `rd_len > 0`;
- write-then-read: `wr` present, `rd_len > 0`.

The command path executes explicit SMBus transactions only. It does not edit
the active polling profile at runtime.

### 4.6 Remote command response

Topic: `pmbus/{gw_id}/cmd/response`

```json
{
  "id": "r001",
  "addr": 88,
  "status": "OK",
  "data": [52, 18],
  "exec_ms": 4
}
```

Field rules:
- `id` echoes the request correlation ID.
- `addr` echoes the target address.
- `status` is a PMBus/SMBus result (`OK`, `NACK`, `TIMEOUT`, `PEC`, etc.) or a
  command-layer error (`BAD_JSON`, `BAD_REQUEST`, `UNSUPPORTED`,
  `QUEUE_FULL`).
- `data` is present for successful read transfers.
- `exec_ms` is measured on the gateway with the monotonic FreeRTOS timer.

---

## 5) Timing source

### 5.1 Wall-clock timestamps (`ts_ms`)

`ts_ms` is **Unix epoch milliseconds (UTC)**, synchronised via SNTP (lwIP)
after the first Wi-Fi connection.  NTP servers: `pool.ntp.org`,
`time.google.com`.  Re-sync interval: configured via `SNTP_UPDATE_DELAY`
in `lwipopts.h` (default 3 600 000 ms = 1 hour).

If NTP is unreachable at boot, `ts_ms` falls back to
milliseconds-since-FreeRTOS-start until the first successful sync.
Once synced, subsequent values are wall-clock.

### 5.2 Monotonic timing fields

All timing/latency fields (`timing_ms.*`, `timing_rolling_ms.*`, `window_ms`)
MUST be produced by the gateway using a **monotonic millisecond timer**
(FreeRTOS tick).  Consumers MUST NOT infer latency from receive timestamps.
