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

---

## 2) QoS / retain policy (MVP)

| Stream    | QoS | retain |
|----------|-----|--------|
| telemetry| 1   | false  |
| status   | 1   | false  |
| events   | 1   | false  |
| metrics  | 0   | false  |

Notes:
- QoS1 may produce duplicates after reconnects; duplicates are handled by deduplication policy below.
- `retain=false` for all streams to avoid stale state surprises.

---

## 3) Deduplication policy (MVP)

Duplicates **are acceptable**. Consumers MUST deduplicate by key:

`(gw_id, addr, seq)`

Rules:
- `seq` is monotonic per gateway instance.
- `seq` resets to 0 on reboot. Consumers MUST deduplicate within a single run.
- Cross-reboot deduplication is NOT supported.

---

## 4) Payload schemas

All payloads are JSON.

### 4.1 Telemetry payload (single snapshot)

Topic: `.../telemetry`

```json
{
  "ts_ms": 1730000000000,
  "time_synced": true,
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
    "telemetry_enqueued": 98
  },
  "gauges": {
    "buffer_depth_ram": 120,
    "buffer_depth_flash": 3500,
    "wifi_rssi_dbm": -56,
    "uptime_s": 1843
  },
  "timing_ms": {
    "read_to_publish_avg": 18.2,
    "read_to_publish_p95": 34.0,
    "read_to_publish_max": 61.0,
    "pmbus_txn_avg": 6.4,
    "pmbus_txn_max": 19.0,
    "mqtt_publish_avg": 4.1,
    "mqtt_publish_max": 20.0
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
- `p95` is computed over a ring buffer of the last N latency samples (recommended N=200).

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
- `BUFFER_OVERFLOW`, `QUEUE_OVERFLOW`

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

All timing/latency fields (`read_to_publish_*`, `pmbus_txn_*`, `mqtt_publish_*`,
`window_ms`) MUST be produced by the gateway using a **monotonic millisecond
timer** (FreeRTOS tick).  Consumers MUST NOT infer latency from receive
timestamps.
