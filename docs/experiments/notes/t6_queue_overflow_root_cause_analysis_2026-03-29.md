# T-6 Queue Overflow Root Cause Analysis

## Purpose

This note explains why `QUEUE_OVERFLOW` still occurred during the `T-6`
blackout/reboot experiment even though the gateway was running with the QSPI
persistent backend enabled.

It is intended as a technical review package for deeper design discussion.

Related evidence:

- `docs/experiments/notes/t6_qspi_blackout_reboot_results_2026-03-29.md`
- `scripts/logs/t6_blackout`

## Executive summary

The short version is:

- **QSPI is working**
- **QSPI persistence across reboot is proven**
- **records are still lost before they reach QSPI**

The current buffering path is layered like this:

1. PMBus poll task pushes telemetry into a small FreeRTOS telemetry queue
2. if that queue is full, records are rescued into a small emergency RAM ring
3. only later, the MQTT task drains those structures into `buffer_mgr`
4. `buffer_mgr` first stores into a RAM ring
5. only when the RAM ring is full do records spill into QSPI

So the QSPI backend is **not the first landing zone** for produced telemetry.
It is the **last** tier in a longer pipeline.

As a result:

- prolonged outage can overflow the upstream queues before the MQTT task has
  time to evacuate them
- reboot during outage can also lose any backlog that is still sitting in the
  volatile RAM ring tier and has not yet spilled into QSPI

## What the `T-6` run proved

From the run artifacts and UART:

- QSPI backend was active
- the gateway recovered `count=235` buffered records after reboot
- those recovered records were flushed after reconnect

This means:

- **QSPI persistence works**

But the same run also showed:

- `QUEUE_OVERFLOW` events for `telemetry_queue`
- UART warnings:
  - `telemetry queue and emergency ring full`
  - `status queue full`

This means:

- **upstream producer-side queues still overflow under a long enough outage**

Therefore the `T-6` result is:

- `PASS for QSPI persistence across reboot`
- `not PASS for lossless buffering under prolonged outage`

## Observed evidence from the run

### Recovered persistent backlog

UART on second boot:

- `Recovered seq=236, count=235 (Active Sec=0)`

`metrics.jsonl` later shows:

- `buffer_dequeued = 279`
- `buffer_enqueued = 44`

Interpretation:

- `279 - 44 = 235`
- the post-reboot run consumed exactly the same amount that UART reported as
  recovered from QSPI

So the persistent replay path is real and internally consistent.

### Overflow evidence

In `scripts/logs/t6_blackout/events.jsonl`:

- lines 3 through 8 are `QUEUE_OVERFLOW` with `detail="telemetry_queue"`

UART also shows:

- `telemetry queue and emergency ring full`
- `status queue full`

So the overflow is not speculative. It happened in the actual run.

## The current runtime data path

### Telemetry production path

In `pmbus_poll_task.c`:

- telemetry is first sent to the FreeRTOS telemetry queue
- if that queue is full, the code tries the emergency ring
- if both are full, the record is dropped and `EVT_QUEUE_OVERFLOW` is posted

Relevant logic:

- `rtos_test/source/pmbus_poll_task.c`

Current behavior:

- `xQueueSend(gateway_ipc_telemetry_queue(), &rec, 0)`
- fallback to `emergency_ring_put(&rec)`
- if that also fails:
  - print warning
  - post `EVT_QUEUE_OVERFLOW`
  - increment `metrics_inc_queue_drops()`

This means telemetry can be lost **before** it ever reaches `buffer_mgr` or
the QSPI backend.

### Status production path

In `pmbus_poll_task.c`:

- status records are sent directly to `status_queue`
- if the queue is full, only a warning is printed

Important difference:

- there is **no emergency rescue path** for status

So status is even more fragile than telemetry during long outage windows.

### Queue evacuation path

In `mqtt_gw_task.c`:

- the MQTT task is responsible for draining:
  - emergency ring
  - telemetry queue
  - status queue
  - event queue
- drained records are encoded to JSON and passed to `buffer_mgr_put()`

Relevant logic:

- `rtos_test/source/mqtt_gw_task.c`
- function `drain_queues_to_buffer()`

This is the crucial design point:

- upstream queues are not evacuated by a dedicated spill task
- they are evacuated by the same task that also handles:
  - MQTT reconnect
  - publish path
  - flush logic

### `buffer_mgr` path

In `buffer_mgr.c`:

- `buffer_mgr_put()` first stores into the RAM ring
- only when the RAM ring is full does it spill to the persistent backend

Relevant logic:

- `rtos_test/source/buffer_mgr.c`

This means that even after a record reaches `buffer_mgr`, it still is not
guaranteed to be in QSPI yet.

## Why QSPI did not prevent overflow

Because QSPI is **downstream** of multiple smaller volatile buffers.

The QSPI backend only protects records that have already passed through:

1. telemetry/status/event queue
2. emergency ring for telemetry
3. MQTT task drain
4. `buffer_mgr` RAM tier saturation

If a record dies in any earlier stage, QSPI never sees it.

## Capacity math for the current profile

Current default profile:

- 2 targets
- `poll_period_ms = 500`
- about `4 telemetry records/s`
- `status_period_ms = 10000` per target

Current queue/ring capacities:

- telemetry queue = `64`
- emergency ring = `256`
- status queue = `16`
- event queue = `16`
- `buffer_mgr` RAM ring = `256`
- QSPI tier = `2048`

### Upstream telemetry headroom before drop

Before a telemetry record can be dropped at the producer side, the system has:

- telemetry queue `64`
- emergency ring `256`

Total producer-side rescue capacity:

- `320 telemetry records`

At ~`4 telemetry records/s`, this is only:

- about **80 seconds**

So if the MQTT task cannot drain fast enough for ~80 s, telemetry overflow is
expected.

### Status headroom before drop

Status has:

- status queue `16`
- no rescue ring

Status rate is roughly:

- 2 targets / 10 s = `0.2 status records/s`

So status survives roughly:

- `16 / 0.2 = 80 seconds`

This matches the telemetry timescale surprisingly closely.

Therefore it is entirely plausible that:

- telemetry queue + emergency ring
- and status queue

both begin overflowing around the same long-outage window.

## Additional subtlety: RAM tier is still volatile

`buffer_mgr` is a two-tier structure:

- tier 1 = RAM ring
- tier 2 = persistent backend

This means:

- records that have been drained into `buffer_mgr`
- but are still sitting in the RAM tier

will be lost if the gateway reboots before they spill to QSPI.

So there are really two different loss modes in the current design:

1. **upstream overflow**
   - queue/ring fill before evacuation
2. **volatile intermediate backlog**
   - record reached `buffer_mgr`
   - but did not yet spill to QSPI before reboot

The `T-6` run proved that at least `235` records did spill to QSPI and were
recovered. It does **not** prove that all generated records survived.

## Why the MQTT task becomes a bottleneck

The MQTT task is doing too many roles at once:

- publish live telemetry
- publish status and events
- detect disconnect
- reconnect Wi-Fi/MQTT
- drain queues into `buffer_mgr`
- flush `buffer_mgr`

During outage, this task experiences blocking windows:

- publish attempts that fail
- `cy_mqtt_connect()` retries
- reconnect backoff loops
- JSON encoding and buffer writes

Although `backoff_wait()` tries to drain queues periodically, the overall
architecture still couples queue evacuation to MQTT transport behavior.

That coupling is the core weakness.

## Why `queue_drops = 0` in the published metrics is not contradictory

The run still showed overflow even though the post-reboot metrics window
contains:

- `queue_drops = 0`

This is explainable:

1. the overflow happened on the **first boot**
2. the gateway rebooted before a later metrics publish from that boot could
   successfully reach the broker
3. metrics counters restart after reboot
4. status queue overflow currently does not feed the same counter path as
   telemetry queue overflow

So the trusted loss evidence for this run is:

- UART warnings
- `QUEUE_OVERFLOW` events in `events.jsonl`

not the post-reboot `queue_drops` metric alone.

## Root cause statement

The root cause is:

- **persistent storage is placed too late in the buffering pipeline**
- **queue evacuation is coupled to the MQTT task**

In other words:

- the design does not push records into a durable backlog immediately when the
  transport path becomes unhealthy
- instead, it relies on several bounded volatile structures first

That is why `QUEUE_OVERFLOW` can still occur even with a working QSPI backend.

## Practical fix options

### Option A. Dedicated spill task

Best architectural fix.

Introduce a task whose only job is:

- drain telemetry/status/event queues continuously
- store records into `buffer_mgr`

Benefits:

- decouples queue evacuation from MQTT reconnect/publish stalls
- removes the largest blind spots

Tradeoff:

- more RTOS complexity
- need to reason about ordering carefully

### Option B. Add rescue paths for status/events

Current telemetry has a rescue ring. Status does not.

Add either:

- a status rescue ring
- or direct spill of status into `buffer_mgr`

Benefits:

- removes easy `status_queue full` loss

Tradeoff:

- extra implementation complexity
- may still leave telemetry bottleneck unsolved

### Option C. Spill directly to `buffer_mgr` earlier

Move persistence closer to the producer side.

Examples:

- on queue-full, bypass emergency ring and write directly to `buffer_mgr`
- or, when MQTT is offline, route telemetry directly to `buffer_mgr`

Benefits:

- fewer volatile stages before durable storage

Tradeoff:

- must ensure PMBus poll task does not block on slower storage calls

### Option D. Reduce MQTT blocking windows

Current MQTT timeout:

- `MQTT_TIMEOUT_MS = 5000`

Possible changes:

- reduce timeout to `1000-1500 ms`
- force offline after fewer publish failures

Benefits:

- smaller blind windows

Tradeoff:

- more reconnect churn
- still not a full architectural fix

### Option E. Increase queue/ring sizes

Fast mitigation:

- telemetry queue: `64 -> 128` or `256`
- status queue: `16 -> 32` or `64`
- emergency ring: `256 -> 512` or `1024`

Benefits:

- easy to try
- increases outage tolerance window

Tradeoff:

- only delays overflow
- does not remove the structural bottleneck

### Option F. Reduce producer pressure during outage

Examples:

- lower telemetry poll rate while MQTT is offline
- reduce status rate during offline windows

Benefits:

- fewer records generated while disconnected

Tradeoff:

- lower observability during outage
- still a mitigation, not a root fix

## Recommended order of attack

If the goal is a robust design rather than a local patch, the recommended order
is:

1. add a dedicated spill/evacuation task
2. add rescue for status (and possibly events)
3. reduce MQTT timeout
4. then tune queue/ring sizes if needed

If the goal is a quick experiment-only mitigation, the fastest sequence is:

1. increase queue sizes
2. increase emergency ring size
3. reduce `MQTT_TIMEOUT_MS`

## Questions for OPUS review

1. Should the persistent tier be moved closer to the producer path so that
   generated telemetry reaches durable storage without depending on the MQTT
   task?
2. Is a dedicated spill task the preferred architecture, or should the current
   `mqtt_gw_task` remain responsible for evacuation?
3. Should the RAM tier remain ahead of QSPI during outage, or should offline
   mode switch to direct persistent writes?
4. Should status/events get their own rescue path, or should they share a
   unified spill path with telemetry?
5. For thesis scope, is a mitigation approach acceptable, or should the design
   be corrected architecturally before claiming no-loss blackout behavior?

## Bottom line

The current system is already good enough to prove:

- QSPI persistence works
- buffered data can survive reboot and flush later

But the current architecture is **not yet good enough to guarantee lossless
behavior during a long outage**, because:

- too much volatile buffering exists upstream of QSPI
- queue evacuation depends on a task that is also busy with MQTT transport

That is the real reason `QUEUE_OVERFLOW` still appears in `T-6`.
