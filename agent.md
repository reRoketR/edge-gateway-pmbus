# agent.md — PMBus↔MQTT Edge Gateway (CY8CKIT-062S2-43012) — implementation guide for Copilot

> **Goal (MVP):** Build a Wi‑Fi edge gateway on **CY8CKIT-062S2-43012** that polls **1–2 PMBus/SMBus targets (PSC3M5)** and publishes **telemetry + status + metrics + events** to an MQTT broker. Include **store‑and‑forward** buffering for offline periods and produce **4 reproducible experiments** for the thesis defense.

---

## 0) Project boundaries (do not expand until MVP is done)

### Must have (MVP)
- PMBus master polling on I2C/SMBus with timeout + retries + bus recovery (best‑effort).
- Telemetry snapshot message to MQTT (single message per cycle).
- Status message to MQTT (less frequent).
- Metrics message to MQTT (periodic).
- Events message to MQTT (state changes / notable faults).
- Offline buffering (RAM + persistent storage recommended) and flush when MQTT returns.
- 4 experiments with raw logs and plots.

### Explicitly NOT in MVP
- Local database on the gateway.
- Full PMBus command coverage.
- Dangerous PMBus write commands (e.g., OPERATION) unless added later with strict whitelist & audit.
- Complex UI on the gateway.

---

## 1) Hardware & roles

- **Targets:** 1–2 × PSC3M5 acting as PMBus/SMBus **target/slave** devices.
  - Use Infineon `mtb-pmbus` middleware on PSC3M5 for target functionality.
- **Gateway:** **CY8CKIT-062S2-43012** (PSoC 6 + Wi‑Fi) acting as PMBus/SMBus **controller/master** + MQTT client.

---

## 2) Repository layout (expected)

```
/README.md
/agent.md
/docs/
  architecture.md
  pmbus_command_map.md
  mqtt_topics.md
  experiments/
    methodology.md
    exp1_latency.md
    exp2_throughput.md
    exp3_offline_buffer.md
    exp4_bus_reliability_pec.md
  figures/
/gateway_fw/
  README.md
  src/
  include/
  configs/
  tests/
/target_fw/
  README.md
  device_A/
  device_B/
/scripts/
  mqtt_broker/
  capture/
  plot/
/hw/
  wiring.md
  bom.md
/.github/workflows/
  build_gateway.yml
  build_targets.yml
```

---

## 3) MVP PMBus command set

### 3.1 Targets (PSC3M5) must implement at least
**Identification**
- `PMBUS_REVISION`
- `CAPABILITY`
- `MFR_ID`
- `MFR_MODEL`
- `MFR_REVISION`

**Telemetry (read‑only)**
- `READ_VIN`
- `READ_VOUT`
- `READ_IIN` (optional but recommended)
- `READ_IOUT`
- `READ_TEMPERATURE_1`
- `READ_POUT`

**Status**
- `STATUS_WORD`
- `STATUS_VOUT`
- `STATUS_IOUT`
- `STATUS_TEMPERATURE`

**Optional**
- `CLEAR_FAULTS` (safe; for latched status reset)

### 3.2 Gateway (PSoC) as PMBus master must support
- SMBus read word / block read as needed by command formats
- PEC enabled/disabled (configurable)
- Linear11/Linear16 decode to float SI units

---

## 4) MQTT contracts (topics + payload)

### 4.1 Topics
- Telemetry: `pmbus/<gw_id>/dev/<addr>/telemetry`
- Status:    `pmbus/<gw_id>/dev/<addr>/status`
- Metrics:   `pmbus/<gw_id>/metrics`
- Events:    `pmbus/<gw_id>/events`

Address format: string `"0x58"` (always).

### 4.2 Telemetry payload (one snapshot)
```json
{
  "ts_ms": 1730000000000,
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

**Rules**
- SI units in base units: V, A, °C, W.
- `seq` monotonic per gateway. Persist across reboot if possible.
- `read_ms` = time from first PMBus command start to last PMBus response received (exclude MQTT).

### 4.3 Status payload
```json
{
  "ts_ms": 1730000000000,
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

### 4.4 Metrics payload (periodic, delta counters + gauges + timings)
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
    "buffer_dropped": 0
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

### 4.5 Events payload (state changes)
```json
{ "ts_ms": 1730000000000, "type": "MQTT_DISCONNECTED", "detail": "wifi_lost" }
```

Event types (MVP):
- `MQTT_CONNECTED`, `MQTT_DISCONNECTED`
- `PMBUS_DEVICE_TIMEOUT` (include addr)
- `PMBUS_BUS_RECOVERY`
- `BUFFER_OVERFLOW`

---

## 5) Configuration (MCU-friendly, compile-time profiles)

**YAML/JSON parsing is intentionally NOT used on the gateway MCU.** Configuration is provided as:
- `gateway_config.h` (types + extern)
- `gateway_config.c` (single definition of `const config_t g_config`)
- `configs/profile_*.h` (compile-time profiles for experiments)

### 5.1 Files
- `gateway_fw/include/gateway_config.h`
- `gateway_fw/src/gateway_config.c`
- `gateway_fw/configs/profile_default.h`
- `gateway_fw/configs/profile_exp1_200ms.h`
- `gateway_fw/configs/profile_exp4_pec_off.h`
- etc.

### 5.2 Required config fields (MVP)
- `gw_id`
- I2C/SMBus: `bus`, `speed_hz`, `timeout_ms`, `retries`, `bus_recovery`, `pec_enabled`
- Devices: list of `{addr_7bit, label, poll_period_ms, status_period_ms}`
- MQTT: `host`, `port`, `client_id`, `base_topic`, `qos`, reconnect backoff min/max
- Buffer: `enabled`, `ram_max_records`, `flash_max_records`, `flush_batch_size`, `flush_interval_ms`, `drop_oldest`, `persist_seq`
- `metrics_period_ms`

### 5.3 Skeleton (copy into repo)

**gateway_config.h**
```c
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
  uint8_t  addr_7bit;
  const char *label;
  uint32_t poll_period_ms;
  uint32_t status_period_ms;
} device_cfg_t;

typedef struct {
  const char *gw_id;

  struct {
    uint8_t  bus;
    uint32_t speed_hz;
    uint32_t timeout_ms;
    uint8_t  retries;
    bool     bus_recovery;
    bool     pec_enabled;
  } i2c;

  struct {
    const char *host;
    uint16_t    port;
    const char *client_id;
    const char *base_topic;
    uint8_t     qos;
    uint32_t    backoff_min_ms;
    uint32_t    backoff_max_ms;
  } mqtt;

  struct {
    bool     enabled;
    uint16_t ram_max_records;
    uint32_t flash_max_records;
    uint16_t flush_batch_size;
    uint32_t flush_interval_ms;
    bool     drop_oldest;
    bool     persist_seq;
  } buffer;

  const device_cfg_t *devices;
  uint8_t num_devices;

  uint32_t metrics_period_ms;
} config_t;

extern const config_t g_config;
extern const char *g_profile_name; // defined by active profile
```

**gateway_config.c**
```c
#include "gateway_config.h"
#include "configs/profile_default.h"  // swapped by build flag/profile

const char *g_profile_name = PROFILE_NAME;
const config_t g_config = PROFILE_CONFIG; // defined exactly once
```

**profile_default.h**
```c
#pragma once
#include "gateway_config.h"

static const device_cfg_t k_devices[] = {
  {.addr_7bit=0x58, .label="psu_a", .poll_period_ms=200, .status_period_ms=1000},
  {.addr_7bit=0x59, .label="psu_b", .poll_period_ms=200, .status_period_ms=1000},
};

#define PROFILE_NAME "default"
#define PROFILE_CONFIG ((config_t){   .gw_id = "gw01",   .i2c = {.bus=0, .speed_hz=100000, .timeout_ms=20, .retries=2, .bus_recovery=true, .pec_enabled=true},   .mqtt = {.host="192.168.1.10", .port=1883, .client_id="pmbus-gw01", .base_topic="pmbus/gw01", .qos=1,            .backoff_min_ms=500, .backoff_max_ms=10000},   .buffer = {.enabled=true, .ram_max_records=256, .flash_max_records=20000, .flush_batch_size=50,              .flush_interval_ms=200, .drop_oldest=true, .persist_seq=true},   .devices = k_devices, .num_devices = 2,   .metrics_period_ms = 2000 })
```

### 5.4 Reproducibility requirement (thesis-critical)
At boot, the gateway must log:
- firmware version/commit (if available)
- `g_profile_name`
- key parameters: poll periods, PEC enabled, MQTT host/port, QoS, buffer sizes

Example boot log line:
- `SYS: profile=exp4_pec_off pec=0 poll_ms=200 mqtt=192.168.1.10:1883 qos=1 buf_flash=20000`

Runtime overrides (optional, minimal):
- only network credentials (SSID/pass) and broker host/port may be overridden via NVS/FRAM/UART CLI.
- experimental parameters (poll rate, PEC, buffer sizes) must remain compile-time for repeatability.

---

## 6) RTOS design (minimal, no overengineering)

Use **3 tasks** (4th optional). Prefer static allocation where possible.

### Task A — `pmbus_poll_task` (prio: high-ish)
- Timer-driven polling of devices.
- For each device, build one telemetry snapshot (multiple commands) + push to `telemetry_queue`.
- Separately poll status per `status_period_ms` and push to `status_queue`.
- Push events to `event_queue`.
- Update PMBus counters.

### Task B — `mqtt_task` (prio: medium)
- Maintain MQTT connection (reconnect with backoff).
- Consume `telemetry_queue` and publish.
- On publish fail/offline: enqueue record into buffer (RAM/flash) and increment buffer counters.
- Publish `metrics` every `g_config.metrics_period_ms`.
- Publish events from `event_queue`.

### Task C — `buffer_task` (prio: low-medium)
- Persistent buffer management:
  - append-only log in flash/SD
  - head/tail pointers in FRAM if available
- When MQTT online: flush `flush_batch_size` records per tick.
- Apply `drop_policy` on overflow.

### Task D (optional) — `health_task` (prio: low)
- Kick watchdog, publish heartbeat event, basic self-check.

IPC:
- `telemetry_queue` (records)
- `status_queue` (records)
- `event_queue` (events)
- `mqtt_state` flag (online/offline)

**Hard rule:** no blocking I2C or flash write while holding a mutex needed by MQTT task.

---

## 7) Data structures (for Copilot)

### TelemetryRecord (in C)
- fixed-size struct preferred
- include `ts_ms`, `seq`, `addr`, `label`, values, `read_ms`, `retries`, flags
- MQTT encoding step converts struct → JSON string in a preallocated buffer

### Counters/Gauges
- Use uint32 counters for deltas, reset them each metrics window after publishing
- Use atomic increments or single-writer pattern per task to avoid locks

### Percentiles (p95)
MVP approach:
- keep ring buffer of last N latencies (N=100..200)
- on metrics publish: copy + sort + compute p95 index = `ceil(0.95*N)-1`

---

## 8) Store-and-forward persistence (MVP rules)

- Use append-only record log.
- Each record contains: `seq`, `ts_ms`, type (telemetry/status), minimal fields or prebuilt JSON, CRC.
- Head/tail pointers:
  - store in FRAM (preferred) or in a small flash metadata sector with wear leveling.
- On reboot:
  - recover pointers, validate CRC, continue.

If flash/SD scope is too risky initially:
- MVP fallback: RAM-only queue + clear warning in docs (but this weakens Exp3).

---

## 9) 4 Experiments (defense-friendly)

### Exp1 — End-to-end latency (read→publish)
- Vary poll period: e.g., 500ms vs 200ms
- 1 target vs 2 targets
- Report: p50/p95/p99, max, throughput

### Exp2 — Throughput & stability
- Increase poll rate until failure thresholds:
  - queue growth, rising failures, reconnect storms
- Report: max stable msgs/s, cmd/s, error rates

### Exp3 — Offline buffering (MQTT/Wi‑Fi down)
- Disable Wi‑Fi/MQTT for N minutes
- Show buffer depth growth, drops, recovery time, ordering by seq
- Report: data loss (should be 0 until buffer full), flush speed

### Exp4 — Bus reliability & PEC cost
- PEC on vs off at same poll rate
- Inject NACK/timeout (temporarily disconnect target or delay response)
- Report: error rates, retries, recovery time, latency impact

All experiments must save raw logs as **JSONL**:
- `telemetry.jsonl`, `status.jsonl`, `metrics.jsonl`, `events.jsonl`
- Scripts produce plots from these files.

---

## 10) Scripts (PC side) — minimal

### `scripts/mqtt_broker/docker-compose.yml`
- Mosquitto broker

### `scripts/capture/capture.py`
- Subscribe to:
  - `pmbus/+/dev/+/telemetry`
  - `pmbus/+/dev/+/status`
  - `pmbus/+/metrics`
  - `pmbus/+/events`
- Append each message to corresponding `*.jsonl`.

### `scripts/plot/plot.py`
- Read `metrics.jsonl`
- Produce plots:
  - latency p95 vs time
  - buffer depth vs time (offline window highlighted)
  - errors/retries vs time
  - msgs/s vs time

---

## 11) Definition of Done (MVP acceptance checklist)

Functional:
- [ ] 2 targets run for 30+ min with stable telemetry publish
- [ ] Status topic publishes at configured interval
- [ ] Metrics publish with required fields

Reliability:
- [ ] One target disappears → gateway continues with the other
- [ ] I2C timeouts + retries are observable in metrics
- [ ] Bus recovery event emitted at least once in a test (or documented why not)

Store-and-forward:
- [ ] MQTT/Wi‑Fi down for N minutes → buffer grows, then flushes after reconnect
- [ ] Drop policy is deterministic and logged

Reproducibility:
- [ ] Quickstart in README
- [ ] scripts to capture logs and generate plots
- [ ] experiments docs with exact profile used and steps

---

## 12) Copilot usage rules (copy‑paste prompts)

### General rules to include in every Copilot prompt
- Language: **C** for firmware; no dynamic allocation in hot path.
- Use **preallocated buffers**, fixed-size structs, ring buffers.
- No blocking calls in high-priority tasks beyond configured timeouts.
- Provide unit-testable helper functions for:
  - Linear11/Linear16 decode
  - JSON encoding from structs
  - metrics window aggregation

### Prompt: implement telemetry record struct + JSON encode
> Implement a `TelemetryRecord` struct for PMBus snapshot and a function `encode_telemetry_json(const TelemetryRecord*, char* out, size_t out_sz)` that produces the exact JSON schema from agent.md. Use snprintf, validate buffer size, return length or error. No heap allocations.

### Prompt: implement PMBus polling cycle
> Implement `pmbus_poll_device(device)` that reads commands [READ_VIN, READ_VOUT, READ_IIN, READ_IOUT, READ_TEMPERATURE_1, READ_POUT] with retries+timeout, accumulates total retries and total read_ms, and returns TelemetryRecord + per-command raw hex words (optional). PEC must be configurable via `g_config.i2c.pec_enabled`. Update counters.

### Prompt: implement metrics aggregation
> Implement delta counters, gauges, timing ring buffer for read_to_publish latency, and `publish_metrics()` that outputs the metrics JSON schema. Use N=200 latency samples, compute p95 via copy+sort.

### Prompt: implement persistent buffer (append-only)
> Implement an append-only record log interface: `buffer_put(record)`, `buffer_get_next(record*)`, `buffer_commit_next()`, `buffer_depth()`. Records have CRC. Head/tail pointers persist. Include drop_oldest policy from `g_config.buffer.drop_oldest`.

### Prompt: implement MQTT task state machine
> Implement `mqtt_task` with reconnect backoff (min/max from config), publish telemetry/status/events, and offline behavior: if publish fails, enqueue to buffer and continue. Expose `mqtt_is_online()` flag.

---

## 13) Coding conventions (keep it consistent)
- One module per responsibility:
  - `pmbus_master.c/.h`
  - `pmbus_decode.c/.h`
  - `telemetry.c/.h`
  - `metrics.c/.h`
  - `mqtt_client.c/.h`
  - `buffer.c/.h`
  - `events.c/.h`
  - `gateway_config.h/.c`
- Every public function documented with inputs/outputs/errors.
- Log tags: `PMBUS`, `MQTT`, `BUF`, `METR`, `SYS`
- All “magic numbers” come from config/profile.

---

## 14) Safety note (thesis-friendly)
- MVP is **read‑only** PMBus: no power control writes.
- If later adding writes: strict whitelist, rate limiting, and full audit log.

---

## 15) Quickstart checklist (for README)
- Flash 2 targets, verify they respond on I2C addresses.
- Flash gateway with chosen profile; on boot it must print the profile and key params.
- Run broker (docker-compose).
- Run capture script and confirm telemetry/status/metrics/events are recorded.
- Run plot script to generate figures.

---

**End of agent.md**
