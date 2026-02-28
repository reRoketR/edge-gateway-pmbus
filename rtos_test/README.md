# Gateway Firmware — `pmbus-mqtt-gateway`

FreeRTOS application for CY8CKIT-062S2-43012 that polls PMBus targets over
I²C/SMBus and forwards telemetry, status, metrics, and events to an MQTT broker.

## Build

```bash
make build TOOLCHAIN=GCC_ARM CONFIG=Debug
make program
```

## Test (host-side)

```bash
make test
```

## Source Layout

| Module | Description |
|--------|-------------|
| `source/pmbus_poll_task.c` | Task A — PMBus polling with per-device timers |
| `source/mqtt_gw_task.c` | Task B — MQTT publish with reconnect/backoff |
| `source/buffer_mgr.c` | Task C — RAM + flash store-and-forward buffer |
| `source/pmbus_master.c` | Low-level I²C driver (PDL-based, PEC support) |
| `source/pmbus_decode.c` | Linear11/Linear16 decoders |
| `source/telemetry.c` | JSON encoding for telemetry & status records |
| `source/metrics.c` | Performance counters, timing rings, JSON |
| `source/events.c` | Event records (connect, fault, overflow) + JSON |
| `source/gateway_config.c` | Compile-time config & experiment profiles |

See the [root README](../README.md) for full quickstart and documentation index.
