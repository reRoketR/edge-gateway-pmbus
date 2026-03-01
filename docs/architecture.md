# System Architecture

This document describes the hardware, firmware, and data-flow architecture of the **PMBus↔MQTT Edge Gateway**.

---

## 1 Hardware Overview

```
  ┌─────────────────────────┐        I2C / SMBus (100 kHz)
  │   KIT_PSC3M5_EVK        │  SCL ──────────────────────────── SCL
  │   PMBus Target (Slave)  │  SDA ──────────────────────────── SDA
  │   Address: 0x58         │  GND ──────────────────────────── GND
  │   SCB0, P9_0/P9_2       │
  └─────────────────────────┘
                                           │
                                           ▼
                              ┌──────────────────────────┐
                              │  CY8CKIT-062S2-43012     │
                              │  PSoC 6 + CYW43012 Wi-Fi │
                              │  PMBus Master (Gateway)  │
                              │  SCB3, P6_0/P6_1         │
                              └──────────┬───────────────┘
                                         │ Wi-Fi (2.4 GHz)
                                         ▼
                              ┌──────────────────────────┐
                              │   PC / Laptop            │
                              │   Mosquitto MQTT Broker  │
                              │   192.168.1.2:1883       │
                              │                          │
                              │   capture.py → JSONL     │
                              │   plot.py    → PNG       │
                              └──────────────────────────┘
```

| Board | Role | MCU | I2C Peripheral | Pins |
|-------|------|-----|----------------|------|
| KIT_PSC3M5_EVK | PMBus target (slave) | PSC3 Cortex-M33 | SCB0 | P9_0 (SCL), P9_2 (SDA) |
| CY8CKIT-062S2-43012 | Gateway (master) | PSoC 62 CM4 + CYW43012 | SCB3 | P6_0 (SCL), P6_1 (SDA) |

---

## 2 Firmware Modules

### 2.1 Gateway (rtos_test)

```
source/
├── main.c                 Entry point, BSP init, task creation, scheduler start
├── gateway_config.c/.h    Compile-time config types + profile selection
├── gateway_ipc.c/.h       FreeRTOS queues, seq counter, MQTT-online flag
├── pmbus_master.c/.h      PDL-based I2C master: read_word, read_byte, read_block, PEC
├── pmbus_decode.c/.h      Linear11 / Linear16 decode to milli-units
├── pmbus_poll_task.c/.h   Task A — timer-driven PMBus polling
├── mqtt_gw_task.c/.h      Task B — Wi-Fi + MQTT connect, queue drain, publish
├── buffer_mgr.c/.h        Task C — RAM ring buffer housekeeping + flash spill
├── telemetry.c/.h         TelemetryRecord / StatusRecord structs + JSON encode
├── metrics.c/.h           Delta counters, gauges, latency ring, p95, JSON encode
├── events.c/.h            Event types + JSON encode
├── mqtt_client_config.c   Broker connection info (compile-time defaults)
└── profiles/
    ├── profile_default.h          Baseline: 2s poll, PEC on
    ├── profile_exp1_fast.h        Exp1: 100ms poll, 2 targets
    ├── profile_exp1_single.h      Exp1: 200ms poll, 1 target
    ├── profile_exp2_throughput.h   Exp2: 50ms poll, stress test
    ├── profile_exp3_offline.h     Exp3: normal poll, buffer-focused
    └── profile_exp4_pec_off.h     Exp4: PEC disabled
```

### 2.2 Target (target_proj)

Single-file firmware (`main.c`) that uses the `mtb-pmbus` middleware to act as a PMBus slave device at address 0x58. It simulates a 48 V-in / 12 V-out power supply with slowly-varying sine-wave telemetry and responds to 11 PMBus commands.

---

## 3 RTOS Task Architecture

```
┌──────────────────────────────────────────────────────────┐
│                    FreeRTOS Scheduler                     │
│                                                          │
│  ┌─────────────────┐  ┌─────────────────┐  ┌──────────┐ │
│  │ Task A (prio 4) │  │ Task B (prio 3) │  │ Task C   │ │
│  │ pmbus_poll_task  │  │ mqtt_gw_task    │  │ buffer   │ │
│  │                 │  │                 │  │ (prio 2) │ │
│  │ • PMBus init    │  │ • Wi-Fi connect │  │          │ │
│  │ • Poll devices  │  │ • MQTT connect  │  │ • Buffer │ │
│  │ • Decode L11/16 │  │ • Drain queues  │  │   depth  │ │
│  │ • Push to queue │  │ • Publish JSON  │  │   gauge  │ │
│  │ • Update metrics│  │ • Flush buffer  │  │   update │ │
│  │ • Emit events   │  │ • Pub metrics   │  │          │ │
│  └───────┬─────────┘  └───────┬─────────┘  └────┬─────┘ │
│          │                    │                  │       │
│          ▼                    ▼                  ▼       │
│  ┌─────────────────────────────────────────────────────┐ │
│  │              gateway_ipc (shared state)              │ │
│  │  telemetry_queue [64]   status_queue [16]            │ │
│  │  event_queue [16]       mqtt_online flag             │ │
│  │  seq counter            now_ms() timestamp           │ │
│  └─────────────────────────────────────────────────────┘ │
│                                                          │
│  ┌──────────┐                                            │
│  │ Blinky   │  Heartbeat LED (prio 1)                    │
│  │ (prio 1) │                                            │
│  └──────────┘                                            │
└──────────────────────────────────────────────────────────┘
```

| Task | Name | Priority | Stack | Responsibility |
|------|------|----------|-------|----------------|
| A | `pmbus_poll_task` | 4 (highest) | 1024 words | PMBus polling, decode, enqueue telemetry/status |
| B | `mqtt_gw_task` | 3 | 3072 words | Wi-Fi, MQTT, publish, metrics, reconnect |
| C | `buffer_task` | 2 | 512 words | Buffer housekeeping — gauge metric updates (does NOT publish) |
| — | `blinky_task` | 1 (lowest) | 256 words | Heartbeat LED toggle |

---

## 4 Data Flow

### 4.1 Telemetry Path (hot path)

```
PMBus Target            Gateway MCU                           MQTT Broker
 (I2C Slave)           (I2C Master)                          (Mosquitto)

   ┌──┐       ┌───────────────────────────┐
   │  │ ◄──── │ pmbus_read_word()         │
   │  │ ────► │ pmbus_read_byte()         │
   │  │       │         │                 │
   └──┘       │         ▼                 │
              │ pmbus_linear11_to_milli() │
              │ pmbus_linear16_to_mv()    │
              │         │                 │
              │         ▼                 │
              │ telemetry_record_t        │
              │ (milli-units, no float)   │
              │         │                 │
              │    xQueueSend()           │
              │         │                 │
              │         ▼                 │
              │  ┌──────────────┐         │           ┌──────────┐
              │  │telemetry_queue│────────►│──────────►│ Broker   │
              │  └──────────────┘  JSON   │  MQTT     │ topic:   │
              │                  encode   │  publish  │ .../telem│
              └───────────────────────────┘           └──────────┘
```

### 4.2 Offline Buffering Path

```
                         MQTT offline?
                              │
                    ┌─────────┴──────────┐
                    │ YES                │ NO
                    ▼                    ▼
            ┌──────────────┐    cy_mqtt_publish()
            │  buffer_mgr  │         │
            │  RAM ring    │         ▼
            │  (256 recs)  │      Broker
            └──────┬───────┘
                   │ RAM full?
                   │ YES → flash_buffer_put()
                   │        (spill to Em_EEPROM,
                   │         called in mqtt_gw_task)
                   │
                   │  MQTT back online
                   ▼
            mqtt_gw_task drains via
            flush_buffered_records()
            (peek → publish → consume)
                   │
                   ▼
            cy_mqtt_publish()
```

### 4.3 Metrics Collection

```
pmbus_poll_task ──► metrics_inc_pmbus_reads_ok()
                    metrics_record_pmbus_txn_us()
                         │
mqtt_gw_task   ──► metrics_inc_mqtt_pub_ok()
                    metrics_record_mqtt_publish_us()
                    metrics_record_read_to_publish_us()
                         │
                         ▼
                 metrics_snapshot_and_reset()
                    ├── counters → delta (reset to 0)
                    ├── gauges  → point-in-time snapshot
                    ├── timing  → avg / p95 / max from ring buffer
                    └── rates   → computed from counters ÷ window_ms
                         │
                         ▼
                 encode_metrics_json() → MQTT publish
```

---

## 5 Memory Layout (RAM)

| Component | Size | Notes |
|-----------|------|-------|
| Telemetry queue | 64 × ~78 B = ~5 KB | Survives 128 s of 2 s polling during connect |
| Status queue | 16 × ~28 B = ~448 B | |
| Event queue | 16 × ~56 B = ~896 B | |
| RAM ring buffer | 256 × 594 B = ~149 KB | Pre-encoded JSON + topic per record |
| Metrics latency ring | 200 × 4 B × 3 = ~2.4 KB | 3 rings: read-to-pub, pmbus_txn, mqtt_pub |
| JSON encode buffer | 512 B + 768 B = 1.3 KB | Telemetry/status + metrics (separate) |
| MQTT network buffer | ~2 × 1024 B = ~2 KB | cy_mqtt library |
| Task stacks | (1024+3072+512+256) × 4 = ~19.5 KB | 4 tasks |

**Total firmware: ~940 KB flash, ~180 KB RAM** (out of 2 MB flash / 1 MB RAM available on PSoC 62).

---

## 6 Configuration System

Configuration is **compile-time only** for thesis repeatability. No runtime YAML/JSON parsing.

```
gateway_config.h          Type definitions (config_t, device_cfg_t)
        │
        ▼
gateway_config.c          #include "profiles/profile_<NAME>.h"
        │                 const config_t g_config = PROFILE_CONFIG;
        ▼
Makefile                  GW_PROFILE=exp1_fast → -DGW_PROFILE_HEADER=...
```

Profile switching:
```bash
# Default profile
make build

# Experiment profile
make build GW_PROFILE=exp1_fast
make build GW_PROFILE=exp4_pec_off
```

At boot, `config_print_boot_banner()` logs all active parameters:
```
[SYS] profile=default  pec=1  mqtt=192.168.1.2:1883  qos_data=1  qos_metrics=0
[SYS] i2c: speed=100000  timeout=20ms  retries=2  recovery=0
[SYS] buffer: enabled=1  ram=256  flash=0  batch=50  flush=200ms  drop_oldest=1
[SYS] metrics_period=2000ms
[SYS] devices: 1
[SYS]   [0] 0x58 "psu_a"  poll=2000ms  status=10000ms
```

---

## 7 Build System

- **Toolchain:** GCC ARM (`TOOLCHAIN=GCC_ARM`)
- **IDE:** ModusToolbox 3.7
- **RTOS:** FreeRTOS (bundled via MTB)
- **Libraries:** `mqtt` (Infineon mqtt v4.7.0), `wifi-connection-manager`, `mtb-hal-cat1`, `mtb-pdl-cat1`
- **Target board BSP:** `APP_CY8CKIT-062S2-43012`

```bash
# Build only
make build TOOLCHAIN=GCC_ARM CONFIG=Debug

# Build + flash via KitProg3
make program TOOLCHAIN=GCC_ARM CONFIG=Debug

# Build with experiment profile
make build TOOLCHAIN=GCC_ARM CONFIG=Debug GW_PROFILE=exp1_fast
```
