# PMBus->MQTT Edge Gateway

FreeRTOS-based PMBus/SMBus edge gateway running on `CY8CKIT-062S2-43012`.
The gateway polls PMBus targets, encodes telemetry/status/events/metrics as
JSON, stores records in a two-tier buffer, and publishes through MQTT over
Wi-Fi.

This repository also contains a PMBus target simulator for `KIT_PSC3M5_EVK`,
host-side test coverage, experiment runbooks, and a web dashboard.

## Repository Layout

```text
rtos_test/             Gateway firmware (PSoC 62, FreeRTOS)
  source/              Gateway modules
  tests/               Host-side test suites
  configs/             Wi-Fi credentials (git-ignored)
  bsps/                Gateway BSP

target_proj/           PMBus target simulator firmware
  main.c               Target simulator entry point (default addr 0x58)
  imports/             mtb-pmbus middleware config

scripts/
  capture/             MQTT -> JSONL log capture
  plot/                JSONL -> PNG plots
  mqtt_broker/         Mosquitto example config

pmbus-dashboard-public/
  index.html           Static dashboard UI

docs/
  architecture.md      Hardware, tasks, data flow
  persistent_buffer.md Buffering design and limits
  mqtt_topics.md       Topic and payload reference
  experiments/         Experiment methodology and runbooks
```

Both firmware projects fetch shared MTB libraries into `mtb_shared/` via
`make getlibs`.

## Hardware

| Role | Board | MCU | Bus Pins |
|------|-------|-----|----------|
| Gateway | `CY8CKIT-062S2-43012` | PSoC 62 CM4 + CYW43012 | `P6_0` SCL, `P6_1` SDA |
| Target | `KIT_PSC3M5_EVK` | PSC3 CM33 | `P9_0` SCL, `P9_2` SDA |

Optional SMBALERT wiring for the current target/gateway build:

| Signal | Gateway | Target | Notes |
|--------|---------|--------|-------|
| SMBALERT# | `P5_7` (`CYBSP_D7`) | `P3_0` (`CYBSP_D7`) | Open-drain, add 4.7k pull-up to 3.3 V |

Minimal lab wiring:

```text
Gateway P6_0  --------  Target P9_0   (SCL)
Gateway P6_1  --------  Target P9_2   (SDA)
Gateway P5_7  --------  Target P3_0   (SMBALERT#, optional)
3.3 V --- 4.7k ---+                      (SMBALERT# pull-up)
GND              --------  GND
```

## Quick Start

### 1. Prerequisites

- ModusToolbox 3.7 or newer
- GCC_ARM toolchain
- Python 3.8+
- Wi-Fi credentials in `rtos_test/configs/wifi_config.h`

### 2. Fetch shared libraries

```bash
cd rtos_test && make getlibs && cd ..
cd target_proj && make getlibs && cd ..
```

### 3. Build and flash the target simulator

```bash
cd target_proj
make build TOOLCHAIN=GCC_ARM CONFIG=Debug
make program
```

Expected UART banner ends with:

```text
[TARGET] Waiting for controller reads...
```

### 4. Build and flash the gateway

Default build uses the default profile and the Em_EEPROM persistent backend:

```bash
cd rtos_test
make build TOOLCHAIN=GCC_ARM CONFIG=Debug
make program
```

Expected default boot banner:

```text
[SYS] profile=default  pec=1  mqtt=broker.hivemq.com:1883  q_telem=0  q_ctrl=1  q_metrics=0
[SYS] i2c: speed=100000  transaction_timeout=20ms  retries=2  recovery=0  settle=5ms
[SYS] buffer: enabled=1  ram=256  flash=61  batch=50  drop_oldest=1
[SYS] metrics_period=10000ms
[SYS] filter: telem=ON db=vin:100/vout:20/iin:100/iout:100/temp:1000/pout:1000 hb=10000ms | status=ON on-change init=emit hb=300000ms
[SYS] devices: 1
[SYS]   [0] 0x58 "psu_a"  poll=500ms  status=10000ms
```

QSPI build for larger persistent capacity:

```bash
make build TOOLCHAIN=GCC_ARM CONFIG=Debug BUFFER_BACKEND=QSPI
make program
```

On first use, erase the external flash region:

```bash
make erase MTB_ERASE_EXT_MEM=1
```

## Profiles

Profiles are compile-time only. Switch the active profile with `GW_PROFILE`.

```bash
cd rtos_test
make build TOOLCHAIN=GCC_ARM CONFIG=Debug GW_PROFILE=exp1_fast
```

| Profile | Targets | Poll (ms) | PEC | Purpose |
|---------|---------|-----------|-----|---------|
| `default` | 1 | 500 | on | One gateway + one simulator baseline |
| `exp1_fast` | 2 | 100 | on | Latency stress |
| `exp1_single` | 1 | 200 | on | Single-target latency |
| `exp2_throughput` | 1 | 50 | on | Throughput stress |
| `exp3_offline` | 1 | 500 | on | Offline buffering experiments |
| `exp4_pec_off` | 2 | 200 | off | PEC comparison |
| `raw` | varies | varies | varies | Low-level capture / debug |

The default profile is intentionally single-target (`0x58`) so one gateway
board plus one simulator board works out of the box.

## Runtime Architecture

The runtime is an always-buffered pipeline.

```text
Task A: pmbus_poll_task
  -> gateway_ipc queues
  -> rescue rings on queue overflow

Task C: buffer_task
  -> drains queues and rescue rings
  -> JSON-encodes records
  -> stores into buffer_mgr (RAM + optional persistent tier)

Task B: mqtt_gw_task
  -> sole MQTT publisher
  -> flushes persistent tier first, then RAM
  -> publishes metrics
```

Important properties:

- `buffer_task` is the only upstream consumer of telemetry/status/event queues.
- `mqtt_gw_task` is the only MQTT publisher.
- When RAM is full and persistent buffering is ready, the oldest RAM record is
  migrated to the persistent tier before the new record is admitted to RAM.
  This keeps global publish order FIFO when flushing `persistent -> RAM`.
- Telemetry, status, and event producers each have a rescue path so queue
  overflow does not immediately lose records while buffering is enabled.

More detail: [docs/architecture.md](docs/architecture.md)

## Persistent Buffer Backends

Two compile-time backends are supported through
[`persistent_buffer.h`](rtos_test/source/persistent_buffer.h):

- Em_EEPROM backend (default): internal flash emulation, `61` records
- QSPI backend (`BUFFER_BACKEND=QSPI`): external flash, about `5300` records

The persistent tier is integrity-checked and recovers after reboot, but it is
not documented or treated as transactionally crash-safe in the current design.

More detail: [docs/persistent_buffer.md](docs/persistent_buffer.md)

## MQTT Topics

Representative topics for the default profile:

| Topic | QoS | Content |
|-------|-----|---------|
| `pmbus/thesis_gw01/dev/0x58/telemetry` | 0 | Telemetry samples |
| `pmbus/thesis_gw01/dev/0x58/status` | 1 | PMBus status records |
| `pmbus/thesis_gw01/events` | 1 | Gateway events |
| `pmbus/thesis_gw01/metrics` | 0 | Counters, gauges, timings |

Full payload reference: [docs/mqtt_topics.md](docs/mqtt_topics.md)

## Dashboard and Data Capture

The static dashboard lives in `pmbus-dashboard-public/`.

Serve it locally:

```bash
cd pmbus-dashboard-public
python -m http.server 8080
```

Or use the published GitHub Pages instance if the configured broker and
WebSocket endpoint are reachable from the browser.

The older `scripts/dashboard/` area is not the primary dashboard surface for
the current repository; it only contains auxiliary mock tooling.

Capture MQTT traffic:

```bash
pip install -r scripts/requirements.txt
python scripts/capture/capture.py --host <broker-host> --duration 60
```

Generate plots:

```bash
python scripts/plot/plot.py --log-dir logs/<timestamp>
```

## Tests

Host-side tests:

```bash
cd rtos_test
make test
```

Current host suites:

- `test_buffer_ring`
- `test_pmbus_decode`
- `test_json_encode`
- `test_profile_default`
- `test_i2c_recovery`
- `test_qspi_buffer`
- `test_flash_buffer_layout`
- `test_persistent_seq`
- `test_publish_filter`
- `test_ara`
- `test_integration_offline`

The offline integration suite covers mixed RAM/persistent ordering, batch
limits, and rescue-ring behavior.

## Known Limits

- The current runtime is correctness-focused, not transactionally crash-safe.
  Persistent storage uses integrity checks and best-effort recovery, but this
  pass does not redesign backend commit semantics.
- QSPI capacity is opt-in. Do not assume QSPI-scale buffering in the default
  build.
- MQTT security defaults remain lab-oriented unless you supply a stricter
  broker and credential setup.

## Documentation

| Document | Purpose |
|----------|---------|
| [docs/architecture.md](docs/architecture.md) | Hardware, tasks, and end-to-end flow |
| [docs/persistent_buffer.md](docs/persistent_buffer.md) | Buffer tiers, ordering, and limits |
| [docs/mqtt_topics.md](docs/mqtt_topics.md) | Topic and payload schemas |
| [docs/pmbus_command_map.md](docs/pmbus_command_map.md) | PMBus command set and decoding |
| [docs/experiments/](docs/experiments/) | Experiment methodology and runbooks |

## License

See [LICENSE](LICENSE).
