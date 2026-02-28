# Experiment 4 — Bus Reliability & PEC Cost

## Objective

Compare PMBus communication reliability and performance with PEC (Packet Error
Checking) **enabled** vs **disabled**, and observe how the gateway handles
I²C bus errors (NACK, timeout) and bus recovery.

---

## Hypothesis

- Enabling PEC adds ~1 byte per transaction (CRC-8) and increases per-command
  I²C time by a small constant (~50–100 µs at 100 kHz).
- PEC detects data corruption that would otherwise produce silently wrong
  telemetry values.
- Bus recovery (9× SCL toggles) resolves stuck-SDA conditions and is
  observable in the metrics and events stream.
- Error injection (disconnecting one target mid-run) triggers NACK/timeout
  counters and `PMBUS_DEVICE_OFFLINE` events.

---

## Profiles

### Run A — PEC ON (baseline)

| Parameter | Value |
|-----------|-------|
| Profile | `default` |
| Poll period | 2 000 ms |
| PEC | **ON** |
| Targets | 2 (0x58 + 0x59) |
| Bus recovery | ON |

Build: `make build`

### Run B — PEC OFF

| Parameter | Value |
|-----------|-------|
| Profile | `exp4_pec_off` |
| Poll period | 200 ms |
| PEC | **OFF** |
| Targets | 2 (0x58 + 0x59) |
| Bus recovery | ON |

Build: `make build GW_PROFILE=exp4_pec_off`

---

## Procedure

### Steady-State Comparison (Runs A & B)

1. Connect both targets. Flash gateway with the chosen profile.
2. Start broker and capture (5 minutes): `python scripts/capture/capture.py --duration 300 --out-dir scripts/logs/exp4_<run>/`
3. Verify boot banner shows correct PEC setting.
4. Let the system run for 5 minutes.
5. Stop capture, generate plots.
6. Repeat for the other profile.

### Error Injection (Run C — with Run A profile)

1. Start with both targets connected, PEC ON.
2. Start capture for 5 minutes.
3. At T=1:00 — **physically disconnect Target B** (addr 0x59) by pulling its SDA/SCL jumper wires.
4. At T=3:00 — **reconnect Target B**.
5. At T=5:00 — stop capture.

---

## Key Metrics

### Steady-State (PEC ON vs OFF)

| Metric | Field Path |
|--------|------------|
| PMBus txn avg time | `timing_ms.pmbus_txn_avg` |
| PMBus txn max time | `timing_ms.pmbus_txn_max` |
| PEC failures | `counters_delta.pmbus_crc_pec_fail` |
| Total read failures | `counters_delta.pmbus_reads_fail` |
| Retries | `counters_delta.pmbus_retries` |
| Throughput | `rates.pmbus_cmds_per_s` |

### Error Injection

| Metric | When | Expected |
|--------|------|----------|
| `pmbus_nack` | During disconnect | Increments per poll of Target B |
| `pmbus_timeouts` | During disconnect | May increment |
| `pmbus_retries` | During disconnect | Increments (up to retries × polls) |
| Event: `PMBUS_DEVICE_OFFLINE` | At disconnect | 1 event for addr 0x59 |
| Event: `PMBUS_BUS_RECOVERY` | After timeouts | If bus_recovery triggered |
| Event: `PMBUS_DEVICE_ONLINE` | At reconnect | 1 event for addr 0x59 |
| Target A telemetry | Throughout | Uninterrupted |

---

## Expected Results (Table Template)

### PEC Comparison

| Metric | PEC ON | PEC OFF | Δ |
|--------|--------|---------|---|
| PMBus txn avg (ms) | ___ | ___ | ___ |
| PMBus txn max (ms) | ___ | ___ | ___ |
| PEC failures / window | ___ | N/A | — |
| Throughput (cmd/s) | ___ | ___ | ___ |

### Error Injection

| Metric | Value |
|--------|-------|
| Time to detect disconnect | ___ ms |
| NACK count during 2 min disconnect | ___ |
| Retries during disconnect | ___ |
| Bus recovery events | ___ |
| Target A telemetry gaps | 0 (expected) |
| Time to resume Target B telemetry after reconnect | ___ ms |

---

## Plots

1. **PMBus txn time comparison** — side-by-side box plots for PEC ON vs OFF
2. **Error timeline (error injection)** — NACK, timeout, retries over time with disconnect/reconnect markers
3. **Target A vs B telemetry** — show Target A continuous, Target B gap during disconnect
4. **Bus recovery events** — timestamp markers on the error plot

---

## Analysis Questions

- What is the per-command PEC overhead in milliseconds?
- Does PEC catch any actual corruption during normal operation, or only during
  injected faults?
- How many poll cycles does the gateway take to detect a disconnected target?
- Does Target A experience any disruption when Target B is disconnected
  (shared bus concern)?
- How quickly does the gateway resume normal telemetry after Target B is
  reconnected?
- Does bus recovery succeed in clearing stuck-bus conditions?
