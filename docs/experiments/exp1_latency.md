# Experiment 1 — End-to-End Latency

## Objective

Measure the time from the first PMBus read command to the MQTT publish acknowledgement
(**read-to-publish latency**) under varying poll rates and device counts.

---

## Hypothesis

- Latency is dominated by I²C transaction time at slow poll rates and by
  MQTT/Wi-Fi overhead at fast poll rates.
- Adding a second target roughly doubles per-cycle I²C time but should not
  significantly affect MQTT publish latency.
- At 100 ms poll period with 2 targets, queue backpressure may increase tail latency.

---

## Profiles

### Run A — Fast poll, 2 targets

| Parameter | Value |
|-----------|-------|
| Profile | `exp1_fast` |
| Poll period | 100 ms |
| Status period | 2 000 ms |
| Metrics period | 1 000 ms |
| PEC | ON |
| Targets | 2 (0x58 + 0x59) |
| Bus recovery | ON |

Build: `make build GW_PROFILE=exp1_fast`

### Run B — Moderate poll, 1 target

| Parameter | Value |
|-----------|-------|
| Profile | `exp1_single` |
| Poll period | 200 ms |
| Status period | 5 000 ms |
| Metrics period | 1 000 ms |
| PEC | ON |
| Targets | 1 (0x58) |
| Bus recovery | ON |

Build: `make build GW_PROFILE=exp1_single`

### Run C — Default (baseline)

| Parameter | Value |
|-----------|-------|
| Profile | `default` |
| Poll period | 2 000 ms |
| Status period | 10 000 ms |
| Metrics period | 2 000 ms |
| PEC | ON |
| Targets | 1 (0x58) |

Build: `make build` (no `GW_PROFILE`)

---

## Procedure

1. Flash target(s) and verify PMBus slave ready on UART.
2. Flash gateway with the chosen profile.
3. Start Mosquitto broker.
4. Start capture: `python scripts/capture/capture.py --duration 300 --out-dir scripts/logs/exp1_<run>/`
5. Power on gateway. Verify boot banner shows correct profile.
6. Wait 5 minutes (300 s).
7. Stop capture.
8. Generate plots: `python scripts/plot/plot.py scripts/logs/exp1_<run>/`
9. Repeat for each run (A, B, C).

---

## Key Metrics

From `metrics.jsonl`:

| Metric | Field Path |
|--------|------------|
| Avg latency | `timing_ms.read_to_publish_avg` |
| P95 latency | `timing_ms.read_to_publish_p95` |
| Max latency | `timing_ms.read_to_publish_max` |
| PMBus txn avg | `timing_ms.pmbus_txn_avg` |
| PMBus txn max | `timing_ms.pmbus_txn_max` |
| MQTT pub avg | `timing_ms.mqtt_publish_avg` |
| MQTT pub max | `timing_ms.mqtt_publish_max` |
| Throughput | `rates.telemetry_msgs_per_s` |
| Error rate | `counters_delta.pmbus_reads_fail` / total |

---

## Expected Results (Table Template)

| Run | Profile | Poll (ms) | Targets | Avg (ms) | P95 (ms) | Max (ms) | Msg/s |
|-----|---------|-----------|---------|----------|----------|----------|-------|
| A | exp1_fast | 100 | 2 | ___ | ___ | ___ | ___ |
| B | exp1_single | 200 | 1 | ___ | ___ | ___ | ___ |
| C | default | 2000 | 1 | ___ | ___ | ___ | ___ |

---

## Plots

1. **Latency over time** — `timing_ms.read_to_publish_p95` per metrics window
2. **Latency histogram** — distribution of per-sample latencies (if captured)
3. **Throughput over time** — `rates.telemetry_msgs_per_s`
4. **Error timeline** — retries + timeouts + NACKs per window

---

## Analysis Questions

- Does p95 latency grow when poll period drops below 200 ms?
- How much of the total latency is I²C vs MQTT publish?
- Are there latency spikes correlated with MQTT reconnects or Wi-Fi jitter?
- Does adding a second target cause queue buildup (check `gauges.buffer_depth_ram`)?
