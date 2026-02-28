# PMBus↔MQTT Edge Gateway

A FreeRTOS-based edge gateway running on **CY8CKIT-062S2-43012** (PSoC 62 + Wi-Fi)
that polls PMBus/SMBus targets and publishes telemetry, status, metrics, and events
to an MQTT broker over Wi-Fi.

Designed for thesis-grade reproducible experiments on industrial bus monitoring.

---

## Hardware

| Role | Board | MCU | I²C Pins |
|------|-------|-----|----------|
| **Gateway** | CY8CKIT-062S2-43012 | PSoC 62 CM4 @ 150 MHz | P6_0 (SCL), P6_1 (SDA) |
| **Target** | KIT_PSC3M5_EVK | PSC3 CM33 | P9_0 (SCL), P9_2 (SDA) |

### Wiring

```
Gateway (SCB3)              Target (SCB0)
  P6_0  SCL ────────────── P9_0  SCL
  P6_1  SDA ────────────── P9_2  SDA
  GND   ────────────────── GND

  4.7 kΩ pull-ups on SCL and SDA to 3.3 V
```

---

## Quickstart

### 1. Prerequisites

- [ModusToolbox™ 3.7+](https://www.infineon.com/modustoolbox)
- GNU Arm Embedded Compiler (GCC_ARM)
- [Mosquitto](https://mosquitto.org/download/) MQTT broker
- Python 3.8+ with `pip`

### 2. Flash the Target

```bash
cd target_proj
make build TOOLCHAIN=GCC_ARM CONFIG=Debug
make program
```

Verify on UART: `PMBus slave ready at address 0x58`

### 3. Flash the Gateway

```bash
cd rtos_test
make build TOOLCHAIN=GCC_ARM CONFIG=Debug
make program
```

Verify on UART:
```
===== PMBus-MQTT Edge Gateway =====
  Profile : default
  GW ID   : gw01
  Devices : 1
  PEC     : ON
  Poll    : 2000 ms
  MQTT    : 192.168.1.2:1883
  Buffer  : 256 (RAM)
=======================================
```

### 4. Start the MQTT Broker

```bash
mosquitto -c scripts/mqtt_broker/mosquitto.conf
```

Or with a minimal local config:
```
listener 1883 0.0.0.0
allow_anonymous true
```

### 5. Capture Data

```bash
pip install -r scripts/requirements.txt
python scripts/capture/capture.py --broker 192.168.1.2 --duration 60 --out-dir scripts/logs/run1/
```

### 6. Generate Plots

```bash
python scripts/plot/plot.py scripts/logs/run1/
```

Output: `latency.png`, `buffer.png`, `errors.png`, `throughput.png`, `telemetry.png`

---

## Profile System

Experiments use compile-time configuration profiles. Switch profiles at build time:

```bash
make build TOOLCHAIN=GCC_ARM CONFIG=Debug GW_PROFILE=exp1_fast
```

| Profile | File | Poll (ms) | Targets | PEC | Purpose |
|---------|------|-----------|---------|-----|---------|
| `default` | `profile_default.h` | 2000 | 1 | ON | Baseline |
| `exp1_fast` | `profile_exp1_fast.h` | 100 | 2 | ON | Latency stress |
| `exp1_single` | `profile_exp1_single.h` | 200 | 1 | ON | Single-target latency |
| `exp2_throughput` | `profile_exp2_throughput.h` | 50 | 1 | ON | Max throughput |
| `exp3_offline` | `profile_exp3_offline.h` | 500 | 1 | ON | Offline buffering |
| `exp4_pec_off` | `profile_exp4_pec_off.h` | 200 | 2 | OFF | PEC comparison |

---

## MQTT Topics

| Topic | QoS | Content |
|-------|-----|---------|
| `pmbus/gw01/dev/0x58/telemetry` | 1 | Voltage, current, temperature, power |
| `pmbus/gw01/dev/0x58/status` | 1 | STATUS_WORD, STATUS_VOUT/IOUT/TEMP |
| `pmbus/gw01/metrics` | 0 | Counters, gauges, timing, rates |
| `pmbus/gw01/events` | 1 | State changes (connect/disconnect) |

Full payload schemas: [`docs/mqtt_topics.md`](docs/mqtt_topics.md)

---

## Architecture

Four FreeRTOS tasks:

```
┌─────────────────┐     ┌──────────────┐     ┌──────────────┐
│ Task A: PMBus   │────▶│  FreeRTOS    │────▶│ Task B: MQTT │──▶ Wi-Fi ──▶ Broker
│ Poll  (prio 4)  │     │  Queues      │     │ Pub  (prio 3)│
└─────────────────┘     └──────────────┘     └──────┬───────┘
                                                    │ fail
                                                    ▼
                                             ┌──────────────┐
                                             │ Task C: Buf  │
                                             │ Mgr  (prio 2)│
                                             └──────────────┘
```

Full design: [`docs/architecture.md`](docs/architecture.md)

---

## Documentation

| Document | Description |
|----------|-------------|
| [`docs/architecture.md`](docs/architecture.md) | System architecture, task design, data flow |
| [`docs/pmbus_command_map.md`](docs/pmbus_command_map.md) | PMBus command table, encoding formats, PEC |
| [`docs/mqtt_topics.md`](docs/mqtt_topics.md) | MQTT topic scheme and JSON payload schemas |
| [`docs/persistent_buffer.md`](docs/persistent_buffer.md) | Store-and-forward buffer design |
| [`docs/experiments/methodology.md`](docs/experiments/methodology.md) | Shared experiment methodology |
| [`docs/experiments/exp1_latency.md`](docs/experiments/exp1_latency.md) | Experiment 1: End-to-end latency |
| [`docs/experiments/exp2_throughput.md`](docs/experiments/exp2_throughput.md) | Experiment 2: Throughput & stability |
| [`docs/experiments/exp3_offline_buffer.md`](docs/experiments/exp3_offline_buffer.md) | Experiment 3: Offline buffering |
| [`docs/experiments/exp4_bus_reliability_pec.md`](docs/experiments/exp4_bus_reliability_pec.md) | Experiment 4: Bus reliability & PEC |

---

## Project Structure

```
rtos_test/
├── main.c                          # Entry point, task creation
├── source/
│   ├── gateway_config.c/.h         # Compile-time configuration
│   ├── gateway_ipc.c/.h            # FreeRTOS queues, shared state
│   ├── pmbus_master.c/.h           # I²C/SMBus low-level driver
│   ├── pmbus_decode.c/.h           # Linear11/Linear16 decoders
│   ├── pmbus_poll_task.c/.h        # Task A: PMBus polling
│   ├── mqtt_gw_task.c/.h           # Task B: MQTT publish
│   ├── buffer_mgr.c/.h             # Task C: offline buffer
│   ├── telemetry.c/.h              # Telemetry/status records + JSON
│   ├── metrics.c/.h                # Performance counters + JSON
│   ├── events.c/.h                 # Event records + JSON
│   └── profiles/                   # Experiment configuration profiles
├── scripts/
│   ├── capture/capture.py          # MQTT → JSONL capture
│   ├── plot/plot.py                # JSONL → PNG plots
│   └── requirements.txt            # Python dependencies
├── tests/                          # Unit tests (host-side)
├── docs/                           # Documentation (see table above)
└── Makefile                        # ModusToolbox build system
```

---

## Tests

Host-side unit tests for decode, JSON encoding, and buffer logic:

```bash
# Requires a host-gcc (not cross-compiler)
gcc -o test_decode tests/test_pmbus_decode.c source/pmbus_decode.c -I source -lm && ./test_decode
```

---

## License

See [LICENSE](LICENSE).
