# PMBus-MQTT Edge Gateway - API Reference {#mainpage}

## Introduction

This firmware runs on `CY8CKIT-062S2-43012` and implements a PMBus/SMBus to
MQTT edge gateway. The gateway polls PMBus targets over I2C, encodes
telemetry/status/events/metrics as JSON, stores records in a two-tier buffer,
and publishes through MQTT over Wi-Fi.

The repository also contains a PMBus target simulator for `KIT_PSC3M5_EVK`.

## Architecture Overview

The gateway runtime uses four FreeRTOS tasks:

| Task | Priority | Role |
|------|----------|------|
| `pmbus_poll_task` | 4 | PMBus polling, decode, status sampling, SMBALERT handling |
| `mqtt_gw_task` | 3 | Wi-Fi/MQTT connection and buffered publish |
| `buffer_task` | 2 | Drain IPC queues/rescue rings and store records in `buffer_mgr` |
| `blinky_task` | 1 | Heartbeat LED |

The active runtime is always-buffered:

```text
pmbus_poll_task -> gateway_ipc queues/rescue rings
buffer_task     -> JSON encode -> buffer_mgr
mqtt_gw_task    -> flush persistent tier -> flush RAM tier -> publish
```

## Module Reference

- @ref gateway_config - compile-time configuration and profiles
- @ref gateway_ipc - shared queues, sequence counter, MQTT-online state
- @ref pmbus_master - low-level PMBus/SMBus master driver
- @ref pmbus_decode - Linear11 / Linear16 decoders
- @ref pmbus_poll_task - Task A: periodic PMBus polling
- @ref buffer_mgr - Task C: store-and-forward buffering
- @ref mqtt_gw_task - Task B: Wi-Fi/MQTT connection and publish
- @ref telemetry - telemetry/status structures and JSON encoding
- @ref metrics - counters, gauges, timing, and JSON encoding
- @ref events - event records and JSON encoding

## Configuration Profiles

Build with a specific profile:

```text
make build GW_PROFILE=exp1_fast
```

Current profiles:

- `default` - single-target baseline (`0x58`), 500 ms poll, PEC on
- `exp1_fast` - latency stress, two targets
- `exp1_single` - single-target latency
- `exp2_throughput` - throughput stress
- `exp3_offline` - offline buffering experiments
- `exp4_pec_off` - PEC disabled comparison
- `raw` - low-level capture/debug

## Building

```text
make build TOOLCHAIN=GCC_ARM CONFIG=Debug
make program
```
