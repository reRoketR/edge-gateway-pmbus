# System Architecture

This document describes the current hardware and runtime architecture of the
PMBus->MQTT edge gateway.

## 1. Hardware Overview

```text
KIT_PSC3M5_EVK (target, addr 0x58)
  SCB0 P9_0/P9_2  ---- I2C/SMBus ----  SCB3 P6_0/P6_1
                                            |
                                            v
                               CY8CKIT-062S2-43012 (gateway)
                                            |
                                            v
                                       Wi-Fi / MQTT
                                            |
                                            v
                                      Broker + tooling
```

Optional SMBALERT wiring:

```text
Gateway CYBSP_D7 (P5_7)  ----  Target CYBSP_D7 (P3_0)
                         |
                       4.7k
                         |
                        3.3V
```

## 2. Firmware Modules

### 2.1 Gateway (`rtos_test`)

| Module | Role |
|--------|------|
| `main.c` | BSP init, boot banner, task creation |
| `gateway_config.*` | Compile-time profile selection and config |
| `gateway_ipc.*` | Shared queues, sequence counter, MQTT-online flag |
| `pmbus_master.*` | PMBus/SMBus master transactions, PEC, ARA |
| `pmbus_decode.*` | Linear11 / Linear16 decoding |
| `pmbus_poll_task.*` | Task A: periodic polling, status sampling, SMBALERT handling |
| `buffer_mgr.*` | Task C: queue/rescue draining and two-tier buffering |
| `buffer_flush.*` | Flush helper used by MQTT task |
| `mqtt_gw_task.*` | Task B: Wi-Fi, MQTT, buffered publish, metrics publish |
| `telemetry.*` | JSON encoding for telemetry/status |
| `events.*` | Event types and JSON encoding |
| `metrics.*` | Counters, gauges, latency rings, JSON encoding |
| `emergency_ring.*` | Rescue rings for telemetry/status/events |
| `persistent_buffer.h` | Backend abstraction: Em_EEPROM or QSPI |
| `flash_buffer.*` | Internal persistent backend |
| `qspi_buffer.*` | External persistent backend |

### 2.2 Target (`target_proj`)

`target_proj/main.c` is a PMBus target simulator built on `mtb-pmbus`. The
default simulator address is `0x58`. It provides synthetic telemetry, status
responses, and SMBALERT support for HIL experiments.

## 3. Task Model

The gateway runtime uses four FreeRTOS tasks:

| Task | Priority | Responsibility |
|------|----------|----------------|
| `pmbus_poll_task` | 4 | Poll PMBus devices, decode values, post telemetry/status/events, react to SMBALERT |
| `mqtt_gw_task` | 3 | Maintain Wi-Fi/MQTT, flush buffered data, publish metrics |
| `buffer_task` | 2 | Drain IPC queues and rescue rings, encode records, store into RAM/persistent buffer |
| `blinky_task` | 1 | Heartbeat LED only |

The architecture is intentionally always-buffered:

```text
Task A: pmbus_poll_task
  -> telemetry/status/event queues
  -> rescue rings when a queue is full

Task C: buffer_task
  -> drains queues first
  -> drains rescue rings second
  -> encodes JSON
  -> stores into buffer_mgr

Task B: mqtt_gw_task
  -> flushes persistent tier first
  -> flushes RAM tier second
  -> publishes metrics
```

Key ownership rules:

- `buffer_task` is the sole upstream queue consumer.
- `mqtt_gw_task` is the sole MQTT publisher.
- PMBus producers never publish directly.

## 4. Data Flow

### 4.1 Normal online path

```text
PMBus target
  -> pmbus_poll_task
  -> gateway_ipc queue
  -> buffer_task
  -> buffer_mgr RAM tier
  -> mqtt_gw_task
  -> MQTT broker
```

Even while MQTT is online, data flows through `buffer_mgr`. The runtime does
not bypass the buffer on the hot path.

### 4.2 Queue overflow path

If a producer queue is full:

- telemetry records fall back to the telemetry rescue ring
- status records fall back to the status rescue ring
- event records fall back to the event rescue ring

`buffer_task` drains the normal queues before the corresponding rescue rings so
records rescued later do not overtake records that were already queued.

### 4.3 Persistent spill path

When RAM is full and persistent buffering is enabled:

1. `buffer_mgr` migrates the oldest RAM record into the persistent tier
2. the new record is admitted into RAM

This is the ordering-critical rule introduced in the remediation pass. It keeps
the persistent tier strictly older than the RAM tier.

### 4.4 Reconnect / flush path

When MQTT is available, `mqtt_gw_task` flushes in this order:

1. persistent tier
2. RAM tier

Because only older RAM records are migrated to persistent storage, this
`persistent -> RAM` flush order preserves end-to-end FIFO ordering.

## 5. Buffering Model

### 5.1 RAM tier

The RAM tier is a fixed-size ring of pre-encoded `buffer_record_t` objects.

Properties:

- stores topic + payload + origin timing metadata
- used for all buffered records first
- does not survive reboot

### 5.2 Persistent tier

Two backends are available through `persistent_buffer.h`:

- Em_EEPROM backend (default): internal flash, 61 records
- QSPI backend: external flash, about 5300 records with current record sizes

The selected backend is compile-time only.

### 5.3 Durability scope

Persistent storage is integrity-checked and recovers after reboot, but it is
not claimed to be transactionally crash-safe. The current design intentionally
defers a full commit/journal redesign.

## 6. Metrics and Events

Metrics are updated from the producer, buffer, and publish paths. Relevant
buffer-related outputs include:

- `buffer_depth_ram`
- `buffer_depth_flash`
- `buffer_enqueued`
- `buffer_dequeued`
- `buffer_dropped`
- `storage.backend`
- `storage.total_writes`

Events include connection changes, buffer overflows, and SMBALERT handling.

## 7. Configuration Model

Configuration is compile-time only. `gateway_config.c` instantiates `g_config`
from the selected profile header.

Default profile characteristics:

- one target: `0x58`
- poll period: `500 ms`
- PEC enabled
- RAM buffer: `256` records
- Em_EEPROM persistent capacity: `61` records

This default profile matches the common one-gateway + one-simulator lab setup.

## 8. Build and Test Surfaces

Gateway firmware:

```bash
cd rtos_test
make build TOOLCHAIN=GCC_ARM CONFIG=Debug
```

QSPI gateway build:

```bash
make build TOOLCHAIN=GCC_ARM CONFIG=Debug BUFFER_BACKEND=QSPI
```

Target simulator:

```bash
cd target_proj
make build TOOLCHAIN=GCC_ARM CONFIG=Debug
```

Host-side tests:

```bash
cd rtos_test
make test
```

The host test suite includes offline integration coverage for mixed
RAM/persistent ordering and rescue-ring behavior.
