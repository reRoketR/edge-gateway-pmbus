# Experiment Execution Guide

Step-by-step instructions for running all four thesis experiments and generating results.

---

## Prerequisites

1. **Hardware wired** per [`docs/hw/wiring.md`](hw/wiring.md)
2. **MQTT broker running** — either:
   - Docker: `cd scripts/mqtt_broker && docker-compose up -d`
   - Native: `mosquitto -c scripts/mqtt_broker/mosquitto.conf`
3. **Target firmware flashed** on KIT_PSC3M5_EVK
4. **Python environment** with packages from `scripts/requirements.txt`:
   ```bash
   pip install -r scripts/requirements.txt
   ```
5. **Gateway firmware builds cleanly** (test with `make build`)

---

## Experiment 1 — End-to-End Latency

### Goal
Measure read-to-publish latency under different polling rates and target counts.

### Steps

```powershell
# 1. Build and flash with exp1_single profile (1 device, 200ms poll)
make program TOOLCHAIN=GCC_ARM CONFIG=Debug GW_PROFILE=exp1_single

# 2. Start capture (90 seconds)
cd scripts/capture
python capture.py --host <broker-host> --duration 90 --out-dir ../logs/exp1_single

# 3. Generate plots
cd ../plot
python plot.py --log-dir ../logs/exp1_single --out-dir ../logs/exp1_single

# 4. Repeat with exp1_fast profile (2 devices, 100ms poll)
cd ../..
make program TOOLCHAIN=GCC_ARM CONFIG=Debug GW_PROFILE=exp1_fast

cd scripts/capture
python capture.py --host <broker-host> --duration 90 --out-dir ../logs/exp1_fast

cd ../plot
python plot.py --log-dir ../logs/exp1_fast --out-dir ../logs/exp1_fast
```

### Expected Outputs
- `logs/exp1_single/` and `logs/exp1_fast/` directories with:
  - `telemetry.jsonl`, `status.jsonl`, `metrics.jsonl`, `events.jsonl`
  - `latency_cdf.png`, `buffer_depth.png`, `error_rates.png`, `throughput.png`, `telemetry_timeseries.png`

### Key Metrics to Report
- `read_to_publish_avg_us`, `read_to_publish_p95_us`, `read_to_publish_max_us`
- `read_to_publish_sample_count` (skip zero-sample windows in latency plots)
- `pmbus_txn_avg_us`, `mqtt_publish_avg_us`

---

## Experiment 2 — Throughput Stress Test

### Goal
Determine maximum sustainable message rate before queue overflow or publish failures.

### Steps

```powershell
# 1. Build and flash with exp2_throughput profile (1 device, 50ms poll)
make program TOOLCHAIN=GCC_ARM CONFIG=Debug GW_PROFILE=exp2_throughput

# 2. Capture for 120 seconds (stress test needs longer window)
cd scripts/capture
python capture.py --host <broker-host> --duration 120 --out-dir ../logs/exp2_throughput

# 3. Generate plots
cd ../plot
python plot.py --log-dir ../logs/exp2_throughput --out-dir ../logs/exp2_throughput
```

### Key Metrics to Report
- `telemetry_msgs_per_s_x10` (divide by 10 for actual rate)
- `pmbus_cmds_per_s_x10`
- `mqtt_pub_ok` vs `mqtt_pub_fail`
- `buffer_dropped` (should be 0 if throughput is sustainable)

---

## Experiment 3 — Offline Buffering & Recovery

### Goal
Verify that records are buffered during MQTT outage and flushed on reconnection. With flash persistence enabled, records survive a gateway reboot.

### Steps

```powershell
# 1. Build and flash with exp3_offline profile (flash buffer enabled)
make program TOOLCHAIN=GCC_ARM CONFIG=Debug GW_PROFILE=exp3_offline

# 2. Start long capture (300 seconds = 5 minutes)
cd scripts/capture
python capture.py --host <broker-host> --duration 300 --out-dir ../logs/exp3_offline

# 3. At T=60s: Stop the MQTT broker to simulate outage
#    (In a separate terminal)
docker-compose -f scripts/mqtt_broker/docker-compose.yml stop
#    OR: taskkill /IM mosquitto.exe /F  (if running natively on Windows)

# 4. Wait 120 seconds (gateway buffers records in RAM + flash)
#    Monitor UART output: should show "[BUF] Buffer depth increasing"

# 5. At T=180s: Restart the broker
docker-compose -f scripts/mqtt_broker/docker-compose.yml start
#    OR: mosquitto -c scripts/mqtt_broker/mosquitto.conf

# 6. Observe flush: gateway drains buffered records to broker
#    UART shows "[BUF] Flushed N buffered records"

# 7. At T=240s (optional): Power-cycle the gateway board
#    Unplug and replug USB. Gateway should recover flash records on boot.

# 8. Let capture complete (T=300s)

# 9. Generate plots
cd ../plot
python plot.py --log-dir ../logs/exp3_offline --out-dir ../logs/exp3_offline
```

### Key Metrics to Report
- `buffer_enqueued` during outage window
- `buffer_dequeued` during flush window
- `buffer_dropped` (should be 0 unless outage exceeds capacity)
- `buffer_depth_ram` and `buffer_depth_flash` time series
- Time to flush all buffered records after reconnection

---

## Experiment 4 — Bus Reliability / PEC

### Goal
Compare PMBus read success rate with PEC (CRC-8) enabled vs disabled.

### Steps

```powershell
# 1. Build and flash with default profile (PEC ON)
make program TOOLCHAIN=GCC_ARM CONFIG=Debug

cd scripts/capture
python capture.py --host <broker-host> --duration 90 --out-dir ../logs/exp4_pec_on

cd ../plot
python plot.py --log-dir ../logs/exp4_pec_on --out-dir ../logs/exp4_pec_on

# 2. Build and flash with exp4_pec_off profile (PEC OFF)
cd ../..
make program TOOLCHAIN=GCC_ARM CONFIG=Debug GW_PROFILE=exp4_pec_off

cd scripts/capture
python capture.py --host <broker-host> --duration 90 --out-dir ../logs/exp4_pec_off

cd ../plot
python plot.py --log-dir ../logs/exp4_pec_off --out-dir ../logs/exp4_pec_off
```

### Key Metrics to Report
- `pmbus_reads_ok` vs `pmbus_reads_fail` (both profiles)
- `pmbus_crc_pec_fail` (PEC ON only — should show detected errors)
- `pmbus_retries`, `pmbus_nack`
- Error rate comparison table

---

## 30-Minute Stability Run

### Goal
Verify the system runs without crashes, memory leaks, or degradation over an extended period.

### Steps

```powershell
# 1. Build with default profile
make program TOOLCHAIN=GCC_ARM CONFIG=Debug

# 2. Capture for 1800 seconds (30 minutes)
cd scripts/capture
python capture.py --host <broker-host> --duration 1800 --out-dir ../logs/stability_30min

# 3. Generate plots
cd ../plot
python plot.py --log-dir ../logs/stability_30min --out-dir ../logs/stability_30min
```

### Pass Criteria
- No crash (UART shows continuous output for 30 min)
- `mqtt_pub_fail` rate < 1%
- `buffer_dropped` = 0
- Latency p95 stable (no upward trend)
- `uptime_s` matches wall-clock time

---

## Results Directory Structure

After running all experiments:

```
scripts/logs/
├── exp1_single/
│   ├── telemetry.jsonl
│   ├── status.jsonl
│   ├── metrics.jsonl
│   ├── events.jsonl
│   └── *.png (5 plots)
├── exp1_fast/
│   └── ...
├── exp2_throughput/
│   └── ...
├── exp3_offline/
│   └── ...
├── exp4_pec_on/
│   └── ...
├── exp4_pec_off/
│   └── ...
└── stability_30min/
    └── ...
```

---

## Troubleshooting

| Issue | Solution |
|-------|----------|
| `capture.py` receives no messages | Check broker is running, gateway is connected (UART shows `[MQTT] Connected`) |
| Gateway reboots repeatedly | Stack overflow — increase task stack sizes in headers |
| Flash buffer init fails | First run may need `flash_buffer_erase_all()` — add a boot-time flag or button check |
| Plots show no data | Ensure JSONL files are non-empty, check `--input` path |
| Profile not applied | Verify `GW_PROFILE=name` matches filename in `source/profiles/` |
