# Persistent Buffer (Store-and-Forward)

This document describes the current two-tier buffering design used by the
PMBus->MQTT edge gateway.

## 1. Goals

- Preserve FIFO ordering across reconnects
- Absorb MQTT/Wi-Fi outages without immediate data loss
- Recover persistent backlog after reboot
- Support two persistent backends selected at build time

Non-goals for the current design:

- transactional crash-safe durability
- local database semantics
- arbitrary queries or compaction

## 2. Runtime Architecture

```text
Task A: pmbus_poll_task
  -> IPC queues
  -> rescue rings when a queue overflows

Task C: buffer_task
  -> drains queues first
  -> drains rescue rings second
  -> builds compact buffer records
  -> stores records via buffer_mgr

Task B: mqtt_gw_task
  -> sole publisher
  -> flushes persistent tier first
  -> flushes RAM tier second
```

`buffer_task` is the only writer into `buffer_mgr`. `mqtt_gw_task` is the only
reader/publisher.

## 3. Ordering Model

Global FIFO correctness depends on one rule:

When the RAM ring is full and the persistent tier is available, `buffer_mgr`
migrates the **oldest RAM record** into persistent storage and then inserts the
new record into RAM.

That gives the two tiers these invariants:

- persistent tier contains older records
- RAM tier contains newer records

At reconnect time, `mqtt_gw_task` flushes:

1. persistent tier
2. RAM tier

With the oldest-RAM spill rule in place, this `persistent -> RAM` order is
globally FIFO-correct.

To prevent reordering during overflow migration, `buffer_mgr` temporarily blocks
RAM `peek/consume` while the oldest RAM record is being copied into the
persistent tier.

## 4. Queue and Rescue-Ring Ordering

The gateway has three producer classes:

- telemetry
- status
- events

Each class has:

- a normal FreeRTOS queue
- a rescue ring used only if the queue is full

`buffer_task` drains each queue before its rescue ring:

1. telemetry queue
2. telemetry rescue ring
3. status queue
4. status rescue ring
5. event queue
6. event rescue ring

This prevents rescued records from overtaking records that were already accepted
by the normal queues.

## 5. RAM Tier

The RAM tier is a fixed-size ring of `buffer_record_t` objects.

Each record stores:

- compact binary payload
- record kind / device selector
- origin timing metadata for same-boot latency tracking
- topic and JSON are reconstructed only at publish time

Properties:

- fast
- non-persistent
- first landing zone for all buffered records

## 6. Persistent Tier Abstraction

`persistent_buffer.h` maps a common API to one of two backends:

```bash
make build                               # Em_EEPROM backend
make build BUFFER_BACKEND=QSPI           # QSPI backend
```

Common API:

- `persistent_buffer_init`
- `persistent_buffer_put`
- `persistent_buffer_peek`
- `persistent_buffer_consume`
- `persistent_buffer_depth`
- `persistent_buffer_total_writes`
- `persistent_buffer_erase_all`

## 7. Em_EEPROM Backend (`flash_buffer.c`)

Default backend. Uses internal Em_EEPROM flash storage.

Current practical characteristics:

- persistent capacity: `61` records
- fixed-size row-based format
- suitable for short and medium outage experiments
- survives reboot

The backend validates stored data with record metadata and CRC checks during
read/recovery.

## 8. QSPI Backend (`qspi_buffer.c`)

Optional backend selected with `BUFFER_BACKEND=QSPI`. Uses the external
`S25FL512S` flash device.

Current practical characteristics:

- about `923,476` records for the default telemetry/status mix
- approximately `9d 17h 12m` of offline buffering at the current 1.1 records/s workload
- binary on-flash layout stores only typed fields, not full JSON strings
- metadata journal across dedicated sectors
- QSPI buffer owns sectors `0-254`; sector `255` remains reserved for `qspi_flash_self_test()`
- backend initialization is refused if the linker places data into `.cy_xip`
- larger capacity for long-duration outage experiments
- survives reboot

Like the Em_EEPROM backend, this pass does not alter on-flash commit semantics.

For the current default profile the capacity increase can be written explicitly:

```text
Usable QSPI data region:
  253 sectors x 262,144 B = 66,322,432 B

Binary-backed record sizes:
  telemetry = 16 + 55 + 4 = 75 B
  status    = 16 + 20 + 4 = 40 B
  weighted average (10 telemetry : 1 status)
             = (10 x 75 + 40) / 11
             = 71.8181818 B/record
  capacity   = floor(66,322,432 / 71.8181818)
             = 923,476 records

Previous 6-sector QSPI layout:
  usable data region = 6 x 262,144 = 1,572,864 B
  capacity           = floor(1,572,864 / 71.8181818)
                     = 21,900 records

Capacity gain from the larger QSPI partition:
  923,476 / 21,900 = 42.17x

Outage tolerance at the current 1.1 records/s workload:
  previous layout = 21,900 / 1.1 = 5 h 31 min 49 s
  new layout      = 923,476 / 1.1 = 9 d 17 h 12 min 04 s
```

## 9. Flush Procedure

While MQTT is online, `mqtt_gw_task` calls the buffer flush path:

1. peek oldest persistent record
2. rebuild topic and JSON at publish time
3. publish
4. consume on success
5. repeat until persistent tier empty or publish fails
6. then do the same for the RAM tier

On publish failure, flushing stops and retries later.

## 10. Overflow Policy

If RAM is full and the persistent tier is available:

- migrate the oldest RAM record to persistent storage
- place the new record into RAM

If both tiers are full:

- `drop_oldest = true`: discard the oldest resident record and admit the new one
- `drop_oldest = false`: drop the incoming record

`buffer_dropped` counts the dropped cases.

## 11. Durability Semantics

The current persistent implementation is:

- reboot-recoverable
- integrity-checked
- best-effort

It is **not** documented as transactionally crash-safe.

The QSPI backend already uses a metadata journal and CRC validation. What is
not claimed here is a fully transactional commit protocol with strict
valid-marker semantics for every possible power-loss point. Stronger crash-safe
persistence remains a follow-up story.

## 12. Backend Capacity Caveats

Do not assume QSPI-scale buffering in the default build.

Current expectations:

| Build | Backend | Typical capacity |
|------|---------|------------------|
| default build | Em_EEPROM | 61 records |
| `BUFFER_BACKEND=QSPI` | QSPI flash | about 923,476 records for the default telemetry/status mix |

Profiles intended for long outage experiments should be explicit about QSPI.

## 13. Metrics Integration

The metrics JSON exposes buffer and storage state.

Relevant gauges:

- `buffer_depth_ram`
- `buffer_depth_flash`
- `storage.backend`
- `storage.total_writes`

Current storage shape:

```json
"storage":{"backend":"qspi","total_writes":1234}
```

Counters:

- `buffer_enqueued`
- `buffer_dequeued`
- `buffer_dropped`

## 14. Migration Notes

Switching between Em_EEPROM and QSPI does not migrate old data between
backends. If changing backends:

1. drain buffered records first
2. rebuild with the desired backend flag
3. erase the target backend if needed

This is a backend switch, not a live data migration.
