# Experiment Methodology

This document defines the general methodology shared by all four experiments.

---

## 1 Equipment

| Role | Board | MCU | Notes |
|------|-------|-----|-------|
| Gateway | CY8CKIT-062S2-43012 | PSoC 62 (CM4 @ 150 MHz) | Wi-Fi via CYW43012 |
| Target A | KIT_PSC3M5_EVK | PSC3 (CM33) | PMBus slave addr **0x58** |
| Target B | KIT_PSC3M5_EVK | PSC3 (CM33) | PMBus slave addr **0x59** (optional) |
| Broker | Windows PC | — | Mosquitto 2.1.2 at `<broker-host>:1883` |

### Wiring (I²C / SMBus)

```
Gateway SCB3               Target SCB0
  P6_0  (SCL) ──────────── P9_0  (SCL)
  P6_1  (SDA) ──────────── P9_2  (SDA)
  GND   ──────────────────  GND
```

- External 4.7 kΩ pull-ups on SCL and SDA to 3.3 V.
- Bus speed: 100 kHz (all experiments).

---

## 2 Firmware Builds

### Gateway

```bash
# Default profile
make build TOOLCHAIN=GCC_ARM CONFIG=Debug

# Experiment-specific profile
make build TOOLCHAIN=GCC_ARM CONFIG=Debug GW_PROFILE=exp1_fast
```

Profile files live in `source/profiles/profile_<name>.h`. The Makefile passes
`-DGW_PROFILE_HEADER='"profiles/profile_<name>.h"'` which `gateway_config.c`
includes via `#ifdef GW_PROFILE_HEADER`.

### Target

```bash
cd target_proj
make build TOOLCHAIN=GCC_ARM CONFIG=Debug
```

The target uses a fixed `cmd_table[]` with 11 PMBus commands and sinusoidal
simulation functions. No configuration profiles needed.

---

## 3 Data Capture

### Broker

Start Mosquitto with:

```bash
mosquitto -c mosquitto_dev.conf
```

Configuration (`mosquitto_dev.conf`):

```
listener 1883
allow_anonymous true
```

### Capture Script

```bash
python scripts/capture/capture.py --host <broker-host> --duration 300 --out-dir scripts/logs/<experiment>/
```

This subscribes to all four topic patterns and writes:
- `telemetry.jsonl`
- `status.jsonl`
- `metrics.jsonl`
- `events.jsonl`

### Plot Script

```bash
python scripts/plot/plot.py scripts/logs/<experiment>/
```

Generates: `latency.png`, `buffer.png`, `errors.png`, `throughput.png`, `telemetry.png`.

---

## 4 Boot Banner Verification

Every experiment run must begin by verifying the boot banner on UART:

```
===== PMBus-MQTT Edge Gateway =====
  Profile : <profile_name>
  GW ID   : gw01
  Devices : N
  PEC     : ON/OFF
  Poll    : <poll_period_ms> ms
  MQTT    : <host>:<port>
  Buffer  : <ram_max_records> (RAM)
=======================================
```

This confirms the correct profile is loaded.

---

## 5 Duration & Warm-Up

| Phase | Duration | Purpose |
|-------|----------|---------|
| Warm-up | 30 s | Let MQTT connect, caches populate, PLL lock |
| Measurement | Experiment-specific (see individual docs) | Actual data collection |
| Cool-down | 10 s | Drain remaining queued records |

Raw JSONL files include warm-up and cool-down; analysis scripts should trim
based on `ts_ms` range if needed.

---

## 6 Reproducibility Checklist

Before each experiment run:

- [ ] Correct profile flashed on gateway (verify boot banner)
- [ ] Target(s) flashed and responding (verify UART target banner and controller-read wait message)
- [ ] Broker running and reachable (`mosquitto_pub -t test -m hello`)
- [ ] Capture script started before gateway power-on
- [ ] Previous log directory backed up or cleared
- [ ] Note ambient temperature (for Exp4 PEC analysis)

---

## 7 Counter Accuracy

Delta counters in the metrics module use bare `++` increments (no mutex).
This is safe because each counter is written by exactly one FreeRTOS task — 
except `queue_drops`, which may be incremented from both `pmbus_poll_task`
and `mqtt_gw_task` and is therefore wrapped in `taskENTER_CRITICAL()`.

The snapshot/reset path (`metrics_snapshot_and_reset`) copies and zeroes all
counters inside a critical section, so no increment is lost across a window
boundary.

Conclusion: counters are **exact** under normal load.  Under extreme
preemption pressure a bare `++` could theoretically tear on a non-Cortex-M
core, but on Cortex-M4 a 32-bit aligned store is atomic, so this is safe.

---

## 8 Derived Metrics (Post-Processing)

The following are computed from raw JSONL during analysis:

| Metric | Source | Computation |
|--------|--------|-------------|
| End-to-end latency | `metrics.jsonl` → `timing_ms.read_to_publish_avg/p95/max` | Per metrics window; ignore windows where `timing_samples.read_to_publish_window == 0` |
| Same-sample latency split | `metrics.jsonl` → `timing_ms.telemetry_before_publish_avg` + `timing_ms.telemetry_publish_avg` | Additive decomposition of `read_to_publish_avg`; do not substitute global `pmbus_txn_avg`/`mqtt_publish_avg` |
| Throughput (msg/s) | `metrics.jsonl` → `rates.telemetry_msgs_per_s` | Direct from gateway metrics |
| Error rate (%) | `metrics.jsonl` → `counters_delta` | `reads_fail / (reads_ok + reads_fail) × 100` |
| Buffer occupancy | `metrics.jsonl` → `gauges.buffer_depth_ram` | Time series plot |
| Data loss | `telemetry.jsonl` → `seq` | Count gaps in `seq` monotonic sequence |
