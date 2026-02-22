# Experiment 2 — Throughput & Stability

## Objective

Determine the **maximum sustainable telemetry throughput** of the gateway by
progressively increasing the poll rate until failure thresholds are reached
(queue overflow, rising error rates, MQTT reconnect storms).

---

## Hypothesis

- The gateway can sustain at least 10 telemetry messages/second for a single
  target without significant error growth.
- Throughput is bounded by the MQTT publish path (Wi-Fi RTT, TLS overhead if
  present) rather than by I²C read speed.
- At the failure point, `buffer_depth_ram` will grow monotonically and
  `buffer_dropped` will become non-zero.

---

## Profile

| Parameter | Value |
|-----------|-------|
| Profile | `exp2_throughput` |
| Poll period | **50 ms** (aggressive) |
| Status period | 5 000 ms |
| Metrics period | 1 000 ms |
| PEC | ON |
| Targets | 1 (0x58) |
| Bus recovery | ON |
| Buffer RAM slots | 256 |

Build: `make build GW_PROFILE=exp2_throughput`

---

## Procedure

1. Flash target and verify PMBus slave ready.
2. Flash gateway with `exp2_throughput` profile.
3. Start broker and capture: `python scripts/capture/capture.py --duration 600 --out-dir scripts/logs/exp2/`
4. Let the gateway run for **10 minutes** (600 s).
5. Stop capture, generate plots.
6. Optionally repeat with poll periods of 100 ms, 200 ms, 500 ms for comparison.

---

## Key Metrics

| Metric | Field Path | Threshold |
|--------|------------|-----------|
| Telemetry msg/s | `rates.telemetry_msgs_per_s` | Should match `1000/poll_ms` |
| PMBus cmd/s | `rates.pmbus_cmds_per_s` | 6 cmds × msg/s |
| Buffer depth | `gauges.buffer_depth_ram` | Must stay near 0 for stable |
| Buffer drops | `counters_delta.buffer_dropped` | Must be 0 for stable |
| Queue overflow events | `events.jsonl` type `QUEUE_OVERFLOW` | Must be 0 for stable |
| MQTT pub failures | `counters_delta.mqtt_pub_fail` | Should be ≤ 1% |
| MQTT reconnects | `counters_delta.mqtt_reconnects` | Should be 0 |

---

## Stability Criteria

The system is "stable" at a given poll rate if, over a 5-minute window:

1. `buffer_depth_ram` does not grow monotonically (i.e., it drains back to ~0).
2. `buffer_dropped == 0`.
3. `mqtt_pub_fail / mqtt_pub_ok < 0.01` (< 1% failure rate).
4. No `QUEUE_OVERFLOW` events.
5. `read_to_publish_p95 < 500 ms`.

---

## Expected Results (Table Template)

| Poll (ms) | Target msg/s | Actual msg/s | Stable? | Buffer peak | Drops | P95 (ms) |
|-----------|-------------|-------------|---------|-------------|-------|----------|
| 50 | 20.0 | ___ | ___ | ___ | ___ | ___ |
| 100 | 10.0 | ___ | ___ | ___ | ___ | ___ |
| 200 | 5.0 | ___ | ___ | ___ | ___ | ___ |
| 500 | 2.0 | ___ | ___ | ___ | ___ | ___ |

---

## Plots

1. **Throughput vs time** — `telemetry_msgs_per_s` across the 10-minute run
2. **Buffer depth vs time** — should show flat near zero or monotonic growth
3. **Error rates vs time** — retries, NACK, timeouts stacked
4. **Latency vs time** — p95 should remain bounded for stable rates

---

## Analysis Questions

- What is the maximum stable poll rate for 1 target? For 2 targets?
- At the failure point, which resource is exhausted first: FreeRTOS queue, RAM
  buffer, or MQTT publish bandwidth?
- Does `pmbus_txn_avg` increase under load (indicating I²C contention)?
- Is there a cliff edge or graceful degradation?
