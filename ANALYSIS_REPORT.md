# Comprehensive Repository Analysis — PMBus↔MQTT Edge Gateway

**Date:** 2026-03-31  
**Repository:** `thesis_proj` (commit-level analysis)  
**Analyst:** Claude Opus 4.6 (automated)

---

## Executive Summary

This repository implements a **FreeRTOS-based edge gateway** on a **PSoC 62 (CY8CKIT-062S2-43012)** that polls PMBus/SMBus power supply targets over I²C and publishes telemetry, status, metrics, and events to an MQTT broker via Wi-Fi. A companion **PSC3 target simulator** emulates a 48 V/12 V power supply with sinusoidal telemetry.

### Key Strengths
- **Mature, well-structured firmware** with clear separation of concerns across 4 FreeRTOS tasks
- **Comprehensive documentation** (270+ line architecture doc, full MQTT contract, PMBus command map, wiring guide, BOM)
- **Two-tier store-and-forward buffer** (RAM + QSPI/Em_EEPROM) with power-safe recovery
- **6 host-side unit test suites** (41 test functions, 160+ assertions), all passing
- **Basic CI already present**: GitHub Actions host-tests + dashboard Pages deploy workflow
- **6 compile-time experiment profiles** enabling reproducible thesis research
- **End-to-end tooling**: Python capture, plotting, web dashboard, Docker broker
- **Well-defined PMBus error recovery**: retry loops, 9×SCL bus recovery, controller reset, backoff

### Key Concerns
- **Experiment data incomplete** — profiles and scripts exist, but Exp1–4 results not yet captured
- **Residual ingress overflow risk** — the original T-6 architectural root cause was mitigated by `buffer_task`, but upstream queues remain bounded and only telemetry has a rescue ring
- **MQTT publish latency spikes** — synchronous publish causes 500 ms–2.5 s tail latency
- **Plaintext / anonymous sample deployment path** — current firmware uses no MQTT username/password, and the repo Mosquitto example allows `allow_anonymous true`
- **Persistent sequence counter missing** — `seq` resets to 0 on reboot

---

## 1. Project Overview & Goals

### 1.1 Research Objectives
- Design and implement a real-time edge gateway bridging **industrial PMBus/SMBus** to **MQTT** over Wi-Fi
- Achieve **thesis-grade reproducible experiments** on latency, throughput, offline buffering, and bus reliability
- Demonstrate **store-and-forward** capability during broker/network outages
- Validate **PEC (Packet Error Checking)** impact on bus reliability vs. overhead

### 1.2 Target Hardware

| Role | Board | MCU | Key Peripheral |
|------|-------|-----|----------------|
| Gateway (master) | CY8CKIT-062S2-43012 | PSoC 62 CM4 @ 150 MHz + CYW43012 Wi-Fi | SCB3 I2C (P6_0/P6_1) |
| Target (slave) | KIT_PSC3M5_EVK | PSC3 CM33 | SCB0 I2C (P9_0/P9_2) |

### 1.3 Sub-Project Relationship
- **`rtos_test/`** — Main gateway firmware (FreeRTOS, Wi-Fi, MQTT, PMBus master, buffering)
- **`target_proj/`** — Standalone PMBus target simulator (bare-metal, no Wi-Fi, `mtb-pmbus` middleware)
- **`scripts/`** — Host-side Python tooling (capture, plot, mock, broker config)
- **`pmbus-dashboard-public/`** — Static web dashboard (Chart.js + Paho MQTT over WebSocket)

Both firmware projects share `mtb_shared/` libraries (ModusToolbox auto-managed).

---

## 2. Architecture & Design Analysis

### 2.1 System Architecture

```
┌─────────────────┐  I2C/SMBus   ┌──────────────────┐  Wi-Fi   ┌────────────┐
│  PSC3 Target    │◄────────────►│  PSoC 62 Gateway  │◄────────►│ MQTT Broker│
│  (PMBus Slave)  │  100 kHz     │  (PMBus Master)   │  2.4 GHz │ (Mosquitto)│
│  addr: 0x58     │              │  FreeRTOS 4 tasks │          │ port 1883  │
└─────────────────┘              └──────────────────┘          └─────┬──────┘
                                                                     │
                                                          ┌──────────┴──────────┐
                                                          │  Host PC            │
                                                          │  capture.py → JSONL │
                                                          │  plot.py    → PNG   │
                                                          │  dashboard  → live  │
                                                          └─────────────────────┘
```

### 2.2 RTOS Task Model (4 tasks)

| Task | Priority | Stack | Role |
|------|----------|-------|------|
| `pmbus_poll_task` (A) | 4 (highest) | 4 KB | I2C polling, decode, enqueue telemetry/status |
| `mqtt_gw_task` (B) | 3 | 12 KB | Wi-Fi, MQTT connect/publish, metrics publish, reconnect |
| `buffer_task` (C) | 2 | 6 KB | Dedicated spill: queue drain → JSON encode → buffer_mgr, gauge updates |
| `blinky_task` | 1 (lowest) | 1 KB | Heartbeat LED |

### 2.3 Key Design Patterns

1. **Producer–Consumer via FreeRTOS Queues** — Task A produces telemetry/status records; a dedicated spill task (Task C) drains queues → JSON encodes → stores in buffer_mgr; Task B publishes from buffer
2. **Pre-encoded JSON in Buffer** — Records are JSON-encoded before entering the buffer, avoiding re-serialization on flush
3. **Two-Tier Store-and-Forward** — RAM ring buffer (fast, volatile) + persistent flash (Em_EEPROM or QSPI)
4. **Compile-Time Configuration Profiles** — Eliminates runtime parsing; profiles switch via `GW_PROFILE=` make variable
5. **Validity Bitmask** — Partial reads produce valid JSON (missing fields omitted rather than zero-filled)
6. **Emergency Ring Buffer** — Lock-free SPSC fallback when IPC telemetry queue is full
7. **Deferred Recovery** — Bus recovery and controller reset are armed (not executed immediately) to avoid blocking the polling task

### 2.4 Separation of Concerns Assessment

**Excellent.** Each module has a single responsibility:

| Layer | Modules | Coupling |
|-------|---------|----------|
| Hardware | `pmbus_master.c`, `qspi_flash.c` | PDL only, no RTOS awareness |
| Protocol | `pmbus_decode.c`, `telemetry.c`, `events.c`, `metrics.c` | Pure functions, unit-testable |
| Data | `buffer_mgr.c`, `flash_buffer.c`, `qspi_buffer.c`, `emergency_ring.c` | FreeRTOS primitives only |
| Application | `pmbus_poll_task.c`, `mqtt_gw_task.c` | Glue layer, minimal logic |
| Infrastructure | `gateway_ipc.c`, `gateway_config.c`, `wallclock.c` | System services |

**One concern:** `pmbus_master.c` is the largest file (~800+ lines) and handles I2C init, transfer, recovery, logging, and pin diagnostics. It could benefit from splitting recovery logic into a separate module, but this is minor for a thesis project.

---

## 3. Key Technical Components

### 3.1 RTOS & Real-Time Aspects

**FreeRTOS Integration:**
- Scheduler: `vTaskStartScheduler()` from `main()`
- Tick: Standard 1 ms tick used for `vTaskDelayUntil()` in polling task
- Critical sections: `taskENTER_CRITICAL()` / `taskEXIT_CRITICAL()` for short atomic operations (counter increments, ring buffer operations)
- No mutexes in hot path (avoids priority inversion)

**Determinism:**
- Poll task uses `vTaskDelayUntil()` with 10 ms base tick — good for jitter control
- Per-device deadlines computed independently
- Hard rule: never hold mutex while blocking on I2C

**Concern:** MQTT publish is synchronous and can block 100–2000+ ms. This is the primary source of tail latency. Note: upstream queue draining is already handled by the dedicated spill task (`buffer_task`), isolating MQTT publish path from producers.

### 3.2 Communication Protocols

**PMBus/SMBus (I2C Master):**
- 11 commands supported: VOUT_MODE, STATUS_WORD, STATUS_VOUT/IOUT/TEMP, READ_VIN/VOUT/IIN/IOUT/TEMP1/POUT
- Linear11 (signed 5+11) and Linear16 (unsigned 16 + VOUT_MODE exponent) decoding
- PEC: CRC-8 (polynomial 0x07) computed and verified on all transactions when enabled
- Retry loop: configurable retries with per-error-type metrics
- Bus recovery: 9×SCL toggle on bus fault; SCB disable/re-enable on timeout/not-ready
- VOUT_MODE exponent cached per device; refreshed every 5 s or on failure

**MQTT:**
- Infineon `cy_mqtt` library (based on coreMQTT)
- 4 topic streams: telemetry (QoS 0), status/events (QoS 1), metrics (QoS 0)
- Baseline profiles use plaintext MQTT/1883 with no username/password
- Exponential backoff reconnect (500 ms → 10 s)
- Last Will and Testament (LWT) enabled

### 3.3 Data Management

**RAM Ring Buffer (buffer_mgr.c):**
- Capacity: 256 records × ~600 bytes = ~149 KB
- Thread-safe with critical sections
- Drop policy: drop_oldest (default) or drop_newest

**Persistent Flash (two backends):**

| Backend | Storage | Capacity | Record Size | Endurance |
|---------|---------|----------|-------------|-----------|
| Em_EEPROM (internal) | 32 KB at 0x14000000 | 63 records | 512 B/row | ~6.3 M writes |
| QSPI (external S25FL512S) | 2 MB (6 data sectors) | ~8400 records | Variable | ~600 K sector erases |

**Boot Recovery:**
- Metadata CRC validated on init; corrupted → fresh start
- QSPI: ping-pong metadata journal for power-safe writes
- Flash: head/tail/count clamped to capacity boundaries

### 3.4 Networking

- **Wi-Fi:** CYW43012, WPA2-PSK, 2.4 GHz
- **SNTP:** lwIP SNTP client with `pool.ntp.org` and `time.google.com`; re-sync every 1 hour
- **Timestamps:** `time_synced` flag in every record; fallback to uptime-ms before first NTP sync
- **MQTTWS:** Dashboard connects via WebSocket on port 9001

### 3.5 Security Assessment

| Aspect | Current State | Risk |
|--------|--------------|------|
| MQTT auth | Firmware sends no MQTT username/password; sample Mosquitto config uses `allow_anonymous true` | **HIGH** — anyone on LAN can publish/subscribe |
| TLS | Baseline profiles use plaintext MQTT/1883; secure path is not exercised in current configs | **HIGH** — credentials and telemetry are exposed on the LAN |
| Wi-Fi credentials | In `wifi_config_local.h` (git-ignored) | OK — compile-time only |
| MQTT credentials | None | **HIGH** — no user/password |
| Firmware signing | None | MEDIUM — development boards only |
| Input validation | PMBus data decoded with bounds checks | LOW — read-only protocol |

**Recommendation:** Implement MQTT username/password + ACL as minimum (identified in diploma_implementation_plan.md as 1-day effort). TLS is optional for LAN-only thesis setup but should be documented as future work.

### 3.6 Reliability Features

| Feature | Implementation | Status |
|---------|---------------|--------|
| I2C retry loop | Configurable retries per transaction | ✅ Working |
| 9×SCL bus recovery | On bus fault errors | ✅ Working |
| Controller reset (SCB disable/re-enable) | On timeout/not-ready with bus idle check | ✅ Working |
| Bus backoff | 500 ms global backoff after recovery | ✅ Working |
| Recovery settle delay | Configurable (default 5 ms) | ✅ Working |
| Device offline detection | 3 consecutive failures → backoff mode | ✅ Working |
| MQTT reconnect | Exponential backoff (500 ms → 10 s) | ✅ Working |
| Store-and-forward buffer | RAM + flash with FIFO flush | ✅ Working |
| Dedicated spill task | `buffer_task` drains upstream queues into `buffer_mgr` independently of MQTT | ✅ Working |
| Partial telemetry records | Validity bitmask, omit failed readings | ✅ Working |
| Emergency ring | Lock-free SPSC fallback for queue overflow | ✅ Working |

---

## 4. Build System Analysis

### 4.1 ModusToolbox Configuration

- **IDE:** ModusToolbox 3.7 with GCC ARM toolchain
- **Build System:** GNU Make with MTB recipes (`recipe-make-cat1a` for PSoC 62)
- **Components:** FREERTOS, LWIP, MBEDTLS, SECURE_SOCKETS

### 4.2 Build Targets

```bash
make build                                    # Debug build, default profile
make build GW_PROFILE=exp1_fast               # Experiment 1 profile
make build BUFFER_BACKEND=QSPI                # Use QSPI flash
make program                                  # Build + flash via KitProg3
make test                                     # Host-side unit tests (no board)
make clean                                    # Remove artifacts
```

### 4.3 Profile System

| Profile | Poll (ms) | Targets | PEC | Buffer | Purpose |
|---------|-----------|---------|-----|--------|---------|
| `default` | 500 | 2 | ON | RAM 256 | Baseline development |
| `exp1_fast` | 100 | 2 | ON | RAM 256 | Latency stress test |
| `exp1_single` | 200 | 1 | ON | RAM 256 | Single-target latency |
| `exp2_throughput` | 50 | 1 | ON | RAM 256 | Maximum throughput |
| `exp3_offline` | 500 | 1 | ON | RAM 256 + Flash 63 | Offline buffering |
| `exp4_pec_off` | 200 | 2 | OFF | RAM 256 | PEC overhead comparison |

### 4.4 Dependencies (mtb_shared/)

~35 libraries including: FreeRTOS, lwIP, mbedTLS, cy_mqtt, wifi-connection-manager, mtb-pdl-cat1, mtb-hal-cat1, serial-flash, retarget-io, and others. Managed via `make getlibs`.

### 4.5 Build Optimization Opportunities

1. **Release builds** — `CONFIG=Release` not yet used; would reduce binary size and improve performance (LTO, -O2)
2. **Link-Time Optimization** — Not enabled; could reduce flash footprint significantly
3. **Dead code elimination** — mbedTLS includes more than needed; `mbedtls_user_config.h` partially strips features
4. **Template profiles** — Could use `CONFIG=Release` for final experiment runs

---

## 5. Documentation Review

### 5.1 Documentation Inventory

| Document | Lines | Quality | Coverage |
|----------|-------|---------|----------|
| [architecture.md](docs/architecture.md) | 270+ | **Excellent** | HW/FW architecture, task model, data flow, memory layout, config system |
| [mqtt_topics.md](docs/mqtt_topics.md) | 200+ | **Excellent** | Complete MQTT contract with 4 JSON schemas, QoS, dedup, timing |
| [persistent_buffer.md](docs/persistent_buffer.md) | 212 | **Excellent** | Two-tier buffer design, flash layout, boot recovery, API |
| [pmbus_command_map.md](docs/pmbus_command_map.md) | 160 | **Excellent** | 11 commands, wire format, encoding, PEC, validity masks |
| [smbus_timeout_recovery.md](docs/smbus_timeout_recovery.md) | 80 | **Good** | Future work proposal (hardware timer recovery) |
| [hw/bom.md](docs/hw/bom.md) | 75 | **Good** | Complete BOM with part numbers and costs (~$105 total) |
| [hw/wiring.md](docs/hw/wiring.md) | 100+ | **Good** | Wiring table, pull-ups, bus config, troubleshooting |
| [experiments/methodology.md](docs/experiments/methodology.md) | 171 | **Good** | Equipment, wiring, capture procedure, counter accuracy |
| [experiments/exp1–exp4](docs/experiments/) | ~100 each | **Good** | Hypotheses, profiles, success criteria |
| [agent.md](agent.md) | 200+ | **Excellent** | AI-assisted development specification, full MVP definition |
| [diploma_implementation_plan.md](docs/diploma_implementation_plan.md) | 300+ | **Excellent** | Gap analysis, phase plan, acceptance criteria (Ukrainian) |

### 5.2 Remaining Documentation Gaps

1. **Inline source comments remain sparse** — Doxygen HTML is committed, but generated pages are only as rich as the current source comments
2. **No deployment guide** — Steps scattered between README and various docs
3. **Experiment results missing** — Profiles and scripts exist but no committed result data
4. **Target simulator documentation** — Only `agent.md` and inline comments describe the target firmware
5. **Dashboard architecture** — No documentation beyond `README.md` basics

### 5.3 Documentation Strengths

- ASCII diagrams are exceptionally clear and informative
- Memory layout and flash format documented to byte-level precision
- MQTT contract is a stable, versioned specification suitable for external consumers
- BOM enables full hardware reproducibility
- Bilingual documentation (Ukrainian implementation plan alongside English technical docs)

---

## 6. Experiments & Validation

### 6.1 Experiment Summary

| Exp | Objective | Profile | Duration | Key Metric | Status |
|-----|-----------|---------|----------|------------|--------|
| **Exp1** | End-to-end latency vs. poll rate | exp1_fast / exp1_single / default | 5 min × 3 | read_to_publish p95/max | ⚠️ Data incomplete |
| **Exp2** | Maximum sustainable throughput | exp2_throughput (50 ms) | 10 min | telemetry_msgs_per_s, queue_drops | ⚠️ Data incomplete |
| **Exp3** | Store-and-forward during broker outage | exp3_offline (3 min outage) | 7 min | buffer_enqueued/dequeued, recovery time | ⚠️ Data incomplete |
| **Exp4** | PEC ON vs OFF reliability comparison | exp4_pec_off + error injection | 5 min × 3 | CRC failures, retry count, overhead | ⚠️ Data incomplete |

### 6.2 HIL Validation Stories

| Story | Objective | Status | Key Finding |
|-------|-----------|--------|-------------|
| **T-2** | I²C recovery path routing | ✅ **PASS** | Timeout→controller reset, fault→SCL recovery |
| **T-3** | Broker outage buffering | ⚠️ **Partial** | Transition zone gap: records lost before failover latch |
| **T-4** | Target hotplug detection | ✅ **PASS** (XRES) | Offline detection ~1500 ms, other target unaffected |
| **D2a-1** | QSPI flash bringup | ✅ **PASS** | 64 MB S25FL512S at 25 MHz, thread-safe |

### 6.3 Known Issues from Experiment Notes

1. **T-3 Transition Zone** — During disconnect processing, a brief window existed where mqtt_gw_task still attempted online publish. **Partial fix already merged:** `mqtt_publish_failure_requires_offline()` now forces offline after `NOT_CONNECTED`/`CLOSED` or 3 consecutive `PUBLISH_FAIL` errors. Residual risk: the ~1–2 publish attempts before the latch triggers can still overflow the telemetry queue under aggressive poll rates.

2. **T-6 Residual Ingress Saturation Risk** — The original architectural root cause was fixed when queue draining moved out of `mqtt_gw_task` into the dedicated `buffer_task`. However, upstream ingress remains bounded (telemetry queue 64 + emergency ring 256, status queue 16, event queue 16), so prolonged outage/overload can still cause loss before data reaches the persistent tier.

3. **T-6 Latency Spikes** — Baseline 20–30 ms, but spikes to 500 ms–2.5 s observed. Root cause: synchronous MQTT publish blocking. **Candidates:** reduce MQTT timeout (5000→1000 ms), reduce flush batch (50→8–16).

4. **T-4 USB Unplug** — USB cable removal disturbs shared I²C bus on this board; XRES method is cleaner.

---

## 7. Code Quality Assessment

### 7.1 Coding Standards

| Aspect | Assessment |
|--------|-----------|
| Naming convention | Consistent `snake_case` for functions, `UPPER_CASE` for constants, `s_` prefix for statics |
| File organization | One module per .c/.h pair, clear header guards |
| Include discipline | Minimal includes, no circular dependencies observed |
| Memory management | Stack-only allocation in hot path; heap only at init (ring buffer array) |
| Error handling | Comprehensive error codes (11 PMBus error types), validity masks, graceful degradation |
| Logging | Structured UART output with `[TAG]` prefixes, gated debug logs |

### 7.2 Complexity Hotspots

| File | Lines | Concern |
|------|-------|---------|
| `pmbus_master.c` | ~900 | Largest module; handles init, transfer, recovery, logging, pin diagnostics |
| `mqtt_gw_task.c` | ~714 | Complex lifecycle (Wi-Fi + MQTT + reconnect + flush + metrics) |
| `pmbus_poll_task.c` | ~646 | Many per-device state variables, deadline tracking, offline detection |
| `buffer_mgr.c` | ~445 | Two-tier complexity, task notification, persistent spill, task signaling |

### 7.3 Resource Management

- **No heap allocation after init** — all runtime buffers are stack or statically allocated
- **Flash writes are intentionally blocking** (~16 ms) — acceptable from buffer_task context
- **Stack sizes are well-tuned** — 4 KB for poll task, 12 KB for MQTT task (Wi-Fi+TLS needs it)
- **Total RAM usage: ~179 KB** out of 1 MB available — comfortable headroom
- **Total flash usage: ~940 KB** out of 2 MB available — adequate for Debug builds

### 7.4 Code Quality Score

| Aspect | Score (0-10) |
|--------|-------------|
| Readability | 9 |
| Consistency | 9 |
| Error handling | 8 |
| Testability | 8 |
| Documentation (inline) | 5 |
| Complexity management | 7 |

---

## 8. Hardware Integration

### 8.1 Peripheral Usage

| Peripheral | Board | Configuration |
|------------|-------|---------------|
| SCB3 (I2C Master) | Gateway | 100 kHz, 7-bit addressing, PEC capable |
| SCB0 (I2C Slave) | Target | PMBus target at 0x58 |
| CYW43012 Wi-Fi | Gateway | 2.4 GHz WPA2-PSK |
| SMIF (QSPI) | Gateway | S25FL512S, 25 MHz (optional persistent backend) |
| SCB3 UART | Target | 115200 baud debug output |
| GPIO LED | Both | Heartbeat indicators |
| TCPWM | Target | ~500 ms periodic timer for simulation tick |

### 8.2 GPIO and Pin Configuration

| Signal | Gateway Pin | Target Pin |
|--------|------------|------------|
| SCL | P6_0 (SCB3) | P9_0 (SCB0) |
| SDA | P6_1 (SCB3) | P9_2 (SCB0) |
| GND | GND | GND |

Pull-ups: 4.7 kΩ to 3.3 V (on-board or external).

### 8.3 Power
- Both boards powered independently via USB (KitProg3 debugger port)
- No external power supply required
- Total BOM: ~$105 USD

---

## 9. Testing & Validation Coverage

### 9.1 Host-Side Unit Tests

| Test Suite | File | Tests | Assertions | Coverage |
|-----------|------|-------|------------|----------|
| Buffer Ring | `test_buffer_ring.c` | 11 | 45+ | FIFO, wraparound, overflow policies, truncation |
| PMBus Decode | `test_pmbus_decode.c` | 6 | 25+ | Linear11/16 roundtrip, VOUT_MODE, PEC CRC-8 |
| JSON Encode | `test_json_encode.c` | 9 | 40+ | Telemetry/status/event JSON, metrics, partial validity |
| I2C Recovery | `test_i2c_recovery.c` | 5 | 20+ | Recovery path routing, settle delay, events |
| QSPI Buffer | `test_qspi_buffer.c` | 8 | 24+ | Ring wraparound, metadata journal, reboot recovery |
| Flash Layout | `test_flash_buffer_layout.c` | 2 | 6 | Layout contract, timing metadata round-trip |
| **Total** | **6 suites** | **41** | **160+** | |

> Note: The implementation plan states 185 tests — that number counts individual assertions across the 6 suites, not distinct test functions. The Makefile compiles and runs 6 executable test suites containing 41 test functions total.

### 9.2 What Is Well-Tested

- PMBus Linear11/Linear16 decode/encode (edge cases, roundtrip verification)
- PEC CRC-8 computation (known vectors, self-consistency)
- Ring buffer FIFO mechanics (wrap, overflow, drop policy)
- JSON serialization (all 4 message types, partial validity, buffer bounds)
- I2C recovery path routing (mock-based, all error types)
- QSPI persistent buffer (wraparound, journal recovery, reboot resilience)

### 9.3 What Is NOT Tested

| Gap | Risk | Mitigation |
|-----|------|------------|
| Multi-task contention | MEDIUM | Critical sections used, but no concurrent stress test |
| MQTT publish path | MEDIUM | Relies on Infineon `cy_mqtt` library; no mock MQTT broker test |
| Wi-Fi reconnection | LOW | HIL-tested in practice, not automated |
| Long-duration stress (>10 min) | MEDIUM | Identified in experiment notes; not yet executed |
| Flash corruption injection | LOW | CRC validation exists; not adversarially tested |
| Full end-to-end integration | MEDIUM | Component tests pass; no automated E2E test suite |
| Status/event queue resilience | MEDIUM | Only telemetry has emergency rescue ring |

### 9.4 Test Infrastructure

- Tests compile and run on host (no MCU required) via `make test`
- Mock layers: I2C mock for recovery tests, RAM-backed QSPI mock for flash buffer tests
- **CI pipeline exists:** GitHub Actions workflow (`ci.yml`) runs host-tests on every push/PR to `main` and `feature/**` branches; a separate Pages deploy workflow (`static.yml`) publishes the dashboard
- Firmware cross-compilation job is defined but disabled pending a self-hosted runner with ModusToolbox
- No code coverage measurement tool configured

---

## 10. Issues & Concerns Matrix

| # | Issue | Severity | Impact | Category | Fix Effort |
|---|-------|----------|--------|----------|-----------|
| 1 | **Experiment data not captured** | **CRITICAL** | Thesis defense blocked without results | Data | 3–4 days |
| 2 | **No MQTT authentication** | **HIGH** | Anyone on LAN can intercept/inject data | Security | 1 day |
| 3 | **Residual ingress overflow during long outages / overload** | **HIGH** | Records can still be lost before reaching persistent storage | Reliability | 1–2 days |
| 4 | **MQTT publish latency spikes** (500 ms–2.5 s) | **HIGH** | Breaks real-time latency guarantees | Performance | 1–2 days |
| 5 | **T-3 transition zone** — residual gap after partial fix | **MEDIUM** | ~1–2 publish attempts before failover latch triggers | Reliability | 0.5 day |
| 6 | **CI firmware job disabled** | **LOW** | Host-tests CI exists and runs; firmware cross-compilation disabled pending self-hosted runner | Process | 0.5 day |
| 7 | **Persistent seq counter missing** | **MEDIUM** | Cross-reboot dedup impossible | Reliability | 2 days |
| 8 | **No TLS encryption** | **MEDIUM** | Telemetry visible in plaintext on network | Security | 2–3 days |
| 9 | **No MQTT command/subscribe** | **MEDIUM** | Gateway is publish-only, no runtime control | Feature | 4–5 days |
| 10 | **Status/event queues have no rescue ring** | **LOW** | Status/event loss during overload | Reliability | 1 day |
| 11 | **Inline code comments sparse** | **LOW** | Doxygen output is committed, but source-level doc comments are minimal | Docs | Ongoing |
| 12 | **Default broker = `broker.hivemq.com`** | **LOW** | Reproducibility issues for experiments | Config | 0.5 day |

---

## 11. Project Maturity Assessment

### 11.1 Assessment Scorecard

| Dimension | Score (0-10) | Notes |
|-----------|-------------|-------|
| **Architecture** | 9 | Clean task model, well-separated modules, excellent documentation |
| **Code Quality** | 8 | Consistent style, robust error handling, minimal complexity issues |
| **Testing** | 7 | 6 test suites / 41 test functions / 160+ assertions covering core logic; gaps in integration and stress testing |
| **Documentation** | 8 | Comprehensive technical docs + Doxygen HTML committed; missing experiment results |
| **Build System** | 7 | Working multi-profile builds; CI host-tests active, firmware CI disabled; no Release config exercised |
| **Security** | 3 | No auth, no TLS, anonymous broker — intentional thesis simplification |
| **Reliability** | 7 | Strong recovery paths; queue overflow and publish stalls need fixes |
| **Reproducibility** | 6 | Profiles, scripts, BOM exist; experiment data not yet captured |
| **Tooling** | 8 | Capture, plot, mock, dashboard — complete toolchain |
| **Production-Readiness** | 4 | Thesis-grade, not production — missing auth, TLS, persistent seq, active watchdog supervision, and firmware CI |
| **Overall** | **6.7** | **Strong thesis foundation with identified gaps to address** |

### 11.2 Technical Debt

1. `pmbus_master.c` is too large (~900 lines) — recovery logic could be extracted
2. MQTT timeout is hardcoded at 5000 ms — contributes to latency spikes
3. Emergency ring is telemetry-only — status/events have no fallback
4. Default profile points to public broker — should default to localhost
5. Flash buffer metadata version is 2 but no migration code for version 1
6. `gw_util.c` exists only to work around MinGW `%llu` formatting issues
7. CI firmware cross-compilation job is defined but disabled (`if: ${{ false }}`)

### 11.3 What's Production-Ready

- PMBus I2C master driver with comprehensive error handling
- Two-tier buffer with power-safe flash recovery
- Metrics collection with p95 latency tracking
- Compile-time profile system for repeatable configurations
- JSON encoding with bounds-checking and partial-record support

### 11.4 What's NOT Production-Ready

- No authentication or encryption
- No persistent sequence counter
- No runtime configuration capability (command topic)
- CI firmware job disabled (host-tests CI active)
- No active watchdog supervision loop (boot code only clears the hardware watchdog during startup)
- No firmware OTA update mechanism

---

## 12. Recommendations (Prioritized)

### Quick Wins (< 1 day each)

| # | Action | Impact | Effort |
|---|--------|--------|--------|
| 1 | Enable firmware cross-compilation in CI (self-hosted runner with ModusToolbox) | Firmware regression detection | 0.5 day |
| 2 | Change default broker to `localhost` / LAN IP | Reproducibility | 0.5 hour |
| 3 | Enrich inline code comments (Doxygen output already committed) | Better generated API docs | Ongoing |
| 4 | Reduce MQTT publish timeout from 5000→1000 ms | Latency spike mitigation | 1 hour |
| 5 | Reduce flush batch from 50→16 records | Latency spike mitigation | 1 hour |

### Medium-Term (1–5 days each)

| # | Action | Impact | Effort |
|---|--------|--------|--------|
| 6 | Implement MQTT username/password + ACL | Security minimum | 1 day |
| 7 | Harden T-3 residual gap (tighter failover threshold or pre-publish online check) | Marginal data loss prevention | 0.5 day |
| 8 | Implement persistent seq counter (A/B Em_EEPROM bank) | Cross-reboot dedup | 2 days |
| 9 | Run all 4 experiments + capture data + generate plots | **Thesis deliverable** | 3–4 days |
| 10 | Implement class-based ingress protection (status/event rescue paths and/or queue retuning) | Residual overflow mitigation | 1–2 days |

### Long-Term / Optional

| # | Action | Impact | Effort |
|---|--------|--------|--------|
| 11 | MQTT command topic (subscribe, runtime config) | Feature completeness | 4–5 days |
| 12 | TLS 1.2 with CA certificate | Full security | 2–3 days |
| 13 | Hardware timer SCL-low timeout recovery | Advanced bus recovery | 3–5 days |
| 14 | Status/event rescue ring buffers | Full overflow protection | 1 day |
| 15 | Release build optimization + binary size analysis | Performance | 1 day |

---

## 13. Conclusion

This is a **well-engineered thesis project** with strong architectural foundations, comprehensive documentation, and solid unit test coverage. The firmware design demonstrates graduate-level understanding of embedded systems, real-time task scheduling, industrial protocols, and data reliability patterns.

**The primary blocker for thesis completion is running the experiments and capturing data.** All infrastructure (profiles, scripts, plots, methodology) is in place — the work is execution, not design.

Secondary priorities should focus on **MQTT authentication** (addresses the most obvious defense committee question: "where is the security?") and bounded-ingress reliability tuning (the architectural spill task fix and T-3 early failover latch are already merged, but class-based overload handling is still incomplete).

The codebase is mature enough that remaining issues are tuning problems (latency spikes, queue sizing) rather than architectural deficiencies.

---

*Analysis generated from full repository scan including 35+ source files, 6 test suites, 9 experiment documents, 10 experiment notes, build system configuration, Python tooling, and web dashboard.*
