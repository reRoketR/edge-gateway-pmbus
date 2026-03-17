# PMBus-MQTT Edge Gateway — API Reference {#mainpage}

## Introduction

This firmware runs on the **CY8CKIT-062S2-43012** (PSoC 62 + CYW43012 Wi-Fi)
and implements a real-time PMBus/SMBus → MQTT edge gateway. It polls up to
two PMBus target devices over I²C, decodes telemetry (Linear11/Linear16),
and publishes JSON payloads to an MQTT broker over Wi-Fi.

## Architecture Overview

The system uses four FreeRTOS tasks:

| Task | Priority | Stack | Role |
|------|----------|-------|------|
| pmbus_poll_task | 4 | 1024 words | PMBus I²C polling + telemetry/status queuing |
| mqtt_gw_task    | 3 | 3072 words | Wi-Fi/MQTT connection + JSON publish |
| buffer_task     | 2 | 512 words  | Store-and-forward buffer management |
| metrics (inline)| — | —          | Performance counters (updated from tasks A & B) |

## Module Reference

- @ref gateway_config — Compile-time configuration and profiles
- @ref gateway_ipc — FreeRTOS queues and shared IPC state
- @ref pmbus_master — I²C/SMBus low-level PMBus master driver
- @ref pmbus_decode — Linear11/Linear16 data format decoders
- @ref pmbus_poll_task — Task A: periodic PMBus polling
- @ref mqtt_gw_task — Task B: MQTT connection and publish
- @ref buffer_mgr — Task C: offline store-and-forward ring buffer
- @ref telemetry — Telemetry/status record structures and JSON encoding
- @ref metrics — Performance counters, gauges, timing, and JSON encoding
- @ref events — Event records and JSON encoding

## Configuration Profiles

Build with a specific profile:
```
make build GW_PROFILE=exp1_fast
```

Available profiles:
- `default` — Baseline (500 ms poll, 2 targets, PEC ON)
- `exp1_fast` — Latency stress (100 ms poll, 2 targets)
- `exp1_single` — Single-target latency (200 ms poll)
- `exp2_throughput` — Max throughput (50 ms poll)
- `exp3_offline` — Offline buffer test (500 ms poll)
- `exp4_pec_off` — PEC disabled comparison (200 ms poll)

## Building

```
make build TOOLCHAIN=GCC_ARM CONFIG=Debug
make program
```
