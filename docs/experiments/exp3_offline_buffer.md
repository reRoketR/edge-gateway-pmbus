# Experiment 3 — Offline Buffering

## Objective

Validate the **store-and-forward** mechanism by simulating a broker outage,
observing buffer growth, and measuring data loss and recovery time after
reconnect.

---

## Hypothesis

- During a broker outage, telemetry records accumulate in the RAM ring buffer.
- When the buffer is full, the drop-oldest policy discards the oldest records
  and `buffer_dropped` increments.
- After the broker comes back, buffered records flush within seconds and
  `buffer_depth_ram` returns to zero.
- Sequence numbers (`seq`) in recovered telemetry are monotonic with gaps
  corresponding to dropped records (if any).

---

## Profile

| Parameter | Value |
|-----------|-------|
| Profile | `exp3_offline` |
| Poll period | 500 ms |
| Status period | 5 000 ms |
| Metrics period | 2 000 ms |
| PEC | ON |
| Targets | 1 (0x58) |
| Buffer RAM slots | 256 |
| Drop oldest | true |

Build: `make build GW_PROFILE=exp3_offline`

---

## Procedure

### Timeline

```
T=0:00   Start capture, start gateway  (broker UP)
T=1:00   Verify normal telemetry flow   (warm-up)
T=2:00   STOP broker (simulate outage)
         → Gateway detects disconnect, starts buffering
T=5:00   RESTART broker (3-minute outage)
         → Gateway reconnects, flushes buffer
T=7:00   Stop capture (2 minutes post-recovery)
```

### Steps

1. Flash target and gateway with `exp3_offline` profile.
2. Start broker and capture: `python scripts/capture/capture.py --duration 420 --out-dir scripts/logs/exp3/`
3. At T=2:00, stop broker: `taskkill /IM mosquitto.exe` (Windows) or `Ctrl+C` on broker terminal.
4. At T=5:00, restart broker: `mosquitto -c mosquitto_dev.conf`
5. At T=7:00, stop capture.
6. Generate plots: `python scripts/plot/plot.py scripts/logs/exp3/ --offline-start 120 --offline-end 300`

---

## Key Metrics

| Metric | When | Expected |
|--------|------|----------|
| `gauges.buffer_depth_ram` | During outage | Grows ~2 records/s (poll 500 ms) |
| `gauges.buffer_depth_ram` | After recovery | Drops to 0 within seconds |
| `counters_delta.buffer_enqueued` | During outage | Matches poll rate |
| `counters_delta.buffer_dequeued` | After recovery | Burst of dequeues |
| `counters_delta.buffer_dropped` | If buffer fills | Count of lost records |
| `counters_delta.mqtt_reconnects` | At recovery | Exactly 1 |
| Events: `MQTT_DISCONNECTED` | At outage start | 1 event |
| Events: `MQTT_CONNECTED` | At recovery | 1 event |

### Buffer Capacity Math

- RAM buffer: 256 slots
- Poll period: 500 ms → 2 telemetry records/s
- Outage: 180 s
- Records generated: 180 × 2 = 360
- Buffer capacity: 256
- Expected drops: 360 − 256 = **104 records**

---

## Expected Results (Table Template)

| Metric | Value |
|--------|-------|
| Outage duration | 180 s |
| Records generated during outage | ~360 |
| Buffer peak depth | 256 (full) |
| Records dropped | ~104 |
| Recovery time (reconnect → buffer drained) | ___ s |
| Sequence gaps in telemetry.jsonl | ~104 |
| Data loss rate | ~29% |

---

## Plots

1. **Buffer depth vs time** — ramp during outage, cliff at recovery (use `--offline-start`/`--offline-end` shading)
2. **Enqueue/dequeue rate vs time** — shows buffering burst and flush burst
3. **Sequence number vs time** — should show gap during outage if drops occurred
4. **Events timeline** — `MQTT_DISCONNECTED` / `MQTT_CONNECTED` markers

---

## Variations

| Variation | Change | Purpose |
|-----------|--------|---------|
| Short outage (< buffer capacity) | 60 s outage | Verify zero data loss |
| Long outage (>> buffer capacity) | 10 min outage | Measure steady-state drop rate |
| Larger buffer | `ram_max_records = 1024` | Reduce data loss window |

---

## Analysis Questions

- How quickly does the gateway detect broker loss (time from stop to first `MQTT_DISCONNECTED` event)?
- What is the flush throughput (records/s) after reconnection?
- Are recovered records published in original `seq` order?
- Is there a publish storm after reconnect that could overload the broker?
- If flash persistence were added, could the 180 s outage have zero data loss?
