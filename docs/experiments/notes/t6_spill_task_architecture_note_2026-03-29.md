# T-6 Spill Task Architecture Note (For OPUS Review)

## Purpose

This note explains why the current `T-6` implementation should be finished as a **clean dedicated spill-task architecture**, and why direct upstream queue consumption should be removed from `mqtt_gw_task.c`.

This is a design-review note, not a new experimental result.

Related evidence:
- `docs/experiments/notes/t6_qspi_blackout_reboot_results_2026-03-29.md`
- `docs/experiments/notes/t6_queue_overflow_root_cause_analysis_2026-03-29.md`

## Current State

The current codebase already contains a dedicated spill task in `buffer_mgr.c`:

- `rtos_test/source/buffer_mgr.c:367`
- `rtos_test/source/buffer_mgr.c:389`

That task continuously drains:
- emergency ring
- telemetry queue
- status queue
- event queue

and writes all records into `buffer_mgr`.

At the same time, `mqtt_gw_task.c` still directly consumes the same upstream sources:

- `rtos_test/source/mqtt_gw_task.c:286`
- `rtos_test/source/mqtt_gw_task.c:502`
- `rtos_test/source/mqtt_gw_task.c:577`
- `rtos_test/source/mqtt_gw_task.c:601`

The MQTT task also flushes buffered records from RAM/QSPI:

- `rtos_test/source/mqtt_gw_task.c:293`
- `rtos_test/source/mqtt_gw_task.c:620`

So the system is currently in a **mixed model**:

1. Spill task drains queues into `buffer_mgr`
2. MQTT task still tries to publish directly from queues
3. MQTT task also flushes already-buffered records

## Observable Symptom

In UART logs, this mixed model produces a characteristic pattern:

- initial larger flush after reconnect or boot, for example `Flushed 50 buffered records`
- followed by many tiny flushes such as `Flushed 1 buffered records`, `Flushed 2 buffered records`, `Flushed 3 buffered records`

This behavior is consistent with two parallel ingestion paths:

- some records are published directly from queues by `mqtt_gw_task`
- some records are picked up first by the spill task, buffered, then immediately flushed by MQTT

This is not necessarily a runtime failure, but it is a sign that the ownership model is still ambiguous.

## Why This Matters

The dedicated spill-task idea was introduced to fix a real limitation:

- queue overflow happened **before** records reached QSPI persistence
- queue draining was coupled to MQTT reconnect/publish behavior
- during blackout/outage windows, this caused record loss

The clean solution is:

- upstream producers never depend on MQTT task responsiveness
- one task owns queue evacuation
- one task owns publishing

If the MQTT task still reads the same queues directly, the architecture loses the main benefit of the refactor:

- queue ownership is split
- timing becomes less deterministic
- it becomes harder to reason about whether a record was:
  - published live,
  - buffered first,
  - or racing between both paths

## Root Design Issue

The current code mixes two incompatible models:

### Model A: Direct Live Publish

- poll task pushes to queues
- MQTT task reads queues directly
- MQTT task publishes immediately if online
- buffering is fallback only

### Model B: Dedicated Spill Task

- poll task pushes to queues
- spill task is the sole queue consumer
- spill task writes everything into `buffer_mgr`
- MQTT task is the sole `buffer_mgr` consumer and publisher

Both models are valid individually.

They should **not** be active at the same time.

## Recommended Target Architecture

Adopt the dedicated spill-task model fully:

### Ownership

- `pmbus_poll_task`: producer only
- `buffer_task` / spill task: sole consumer of upstream queues and emergency ring
- `mqtt_gw_task`: sole publisher, reading only from `buffer_mgr` / persistent backend

### Data Path

`Poll/Status/Event producers -> queues/emergency ring -> spill task -> buffer_mgr -> MQTT task -> broker`

### Benefits

- queue evacuation no longer depends on MQTT connect/publish stalls
- clearer reasoning about loss paths
- easier validation of T-6 blackout behavior
- cleaner observability
- simpler concurrency model for persistent buffering

## Tradeoff

The main tradeoff is a small extra hop:

- even live online records first enter `buffer_mgr`
- MQTT publishes them on the next loop instead of directly from queue

In this implementation, the spill task polls every 50 ms:

- `rtos_test/source/buffer_mgr.c:407`

So the expected added latency is modest and usually acceptable for this gateway use case.

For thesis/demo/reliability purposes, this is a better trade than keeping ambiguous dual-consumer behavior.

## What Should Be Removed

To make the architecture clean, `mqtt_gw_task.c` should stop reading upstream queues directly.

That means removing or disabling:

- `process_telemetry_queue()`
- `process_status_queue()`
- `process_event_queue()`

and removing these calls from the main loop:

- `rtos_test/source/mqtt_gw_task.c:286`
- `rtos_test/source/mqtt_gw_task.c:288`
- `rtos_test/source/mqtt_gw_task.c:290`

After that, the MQTT task should do only:

- reconnect / session handling
- `flush_buffered_records()`
- metrics publishing

## Decision Recommendation

For OPUS review, the recommended decision is:

- accept the spill-task direction
- require completion to a **single-consumer upstream model**
- do not intentionally keep the current hybrid model unless low-latency direct publish is a hard requirement

If low-latency direct publish is considered mandatory, then the dedicated spill task should be reconsidered and replaced with a different design.

## Bottom Line

The current refactor is close to the right direction, but the clean architectural endpoint is:

- **spill task owns queue evacuation**
- **MQTT task owns publication**

That separation is the simplest path to making `T-6` understandable, testable, and defensible.
