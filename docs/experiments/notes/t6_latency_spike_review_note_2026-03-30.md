# T-6 Latency Spike Review Note (For External Review)

## Purpose

This note summarizes the current state of the PMBus-MQTT gateway after the QSPI persistence and spill-task refactor, with emphasis on the **remaining end-to-end latency spikes** observed in runtime.

The goal is to give an external reviewer enough context to assess:

- whether the current diagnosis is sound
- whether the observed spikes are expected within the present architecture
- which mitigation path is most justified next

This note is a status/review brief, not a new experimental result.

Related notes:

- `docs/experiments/notes/d2a1_qspi_hal_bringup_results_2026-03-29.md`
- `docs/experiments/notes/t6_qspi_blackout_reboot_results_2026-03-29.md`
- `docs/experiments/notes/t6_queue_overflow_root_cause_analysis_2026-03-29.md`
- `docs/experiments/notes/t6_spill_task_architecture_note_2026-03-29.md`

## Current Architecture

The current runtime data path is:

1. `pmbus_poll_task` produces telemetry and status
2. `gateway_ipc` / queues and telemetry emergency ring hold upstream records briefly
3. `buffer_task` in `rtos_test/source/buffer_mgr.c` is the sole upstream drainer and sole writer into `buffer_mgr`
4. `mqtt_gw_task` in `rtos_test/source/mqtt_gw_task.c` is the sole publisher and flushes only from `buffer_mgr` / persistent storage
5. Persistent buffering uses QSPI (`qspi_buffer.c`) when enabled

The current design is notification-driven at both stages:

- producers notify `buffer_task`
- `buffer_task` notifies `mqtt_gw_task` when it buffered something

This replaced earlier polling-based behavior.

## What Has Already Been Fixed

### 1. QSPI bring-up and boot ordering

Earlier bugs around QSPI access before SMIF init, missing XIP enablement, and early persistent init before scheduler start were fixed.

The system now shows stable boot order:

- QSPI init
- scheduler start
- QSPI self-test
- persistent backend init / recovery

### 2. QSPI persistence across reboot

This was verified on hardware:

- backlog was written to QSPI
- reboot occurred during network outage
- backlog was recovered after reboot
- records were flushed after MQTT reconnect

So the persistence layer itself is considered functionally working.

### 3. Spill-task architecture

The original queue-draining logic inside `mqtt_gw_task` was replaced with a dedicated `buffer_task` that evacuates upstream queues into `buffer_mgr`.

This fixed the earlier architectural issue where queue overflow could happen before records reached the persistence layer.

### 4. Same-boot latency metrics restoration

`read_to_publish_*` metrics were restored after the buffer-first refactor by carrying origin metadata through RAM/QSPI buffering.

The intended semantics are:

- `read_to_publish_*` only applies to records published within the same boot
- records replayed after reboot do not contribute to this metric

### 5. Metrics publish deprioritization

Metrics publishing was changed to **idle-only deferred metrics publishing**:

- metrics are only published when telemetry/control path is idle
- metrics are deferred rather than allowed to interfere with telemetry flush

### 6. Host-test portability fix

`qspi_buffer.c` was updated to use pointer-sized memory-mapped address handling (`uintptr_t`) so `make test` works correctly on 64-bit host builds.

Current host status:

- `make test` passes fully

## Current Observed Runtime Issue

Even after the improvements above, the gateway still occasionally shows large `read_to_publish_*` spikes.

Typical steady-state behavior:

- `avg` often around `20-30 ms`
- `p95` often moderate, then suddenly rises
- `max` sometimes rises to hundreds of milliseconds, and in some runs to `1-2.5 s`

Representative UART behavior in the same periods:

- mostly `Flushed 1 buffered records`
- occasional bursts like `Flushed 6 buffered records`
- sometimes `Flushed 8`, `Flushed 10`, or `Flushed 14`

Representative dashboard CSV excerpt from one run:

- `2026-03-30T14:26:10Z`: `p95=516 ms`, `avg=67.9 ms`, `max=1017 ms`
- `2026-03-30T14:26:50Z`: `p95=1523 ms`, `avg=203.1 ms`, `max=2523 ms`
- `2026-03-30T14:27:00Z`: `p95=1523 ms`, `avg=197 ms`, `max=2523 ms`
- `2026-03-30T14:27:12Z`: `p95=1523 ms`, `avg=229.4 ms`, `max=2523 ms`
- later it falls back down

## Important Interpretation Detail

Latency stats are still computed over a **rolling ring of 100 samples** in `rtos_test/source/metrics.h`, not per metrics publish window.

At the current telemetry rate:

- 2 devices
- poll period `500 ms`
- about `4 telemetry records/s`

that means one latency sample can remain visible in the reported percentile/max set for about `25 s`.

Therefore:

- a short burst of slow samples can remain visible for tens of seconds
- a single old spike can keep `max` high for a while
- if `p95` remains high for longer than expected, that suggests new slow samples are still arriving, not just that the ring has not aged out yet

## Current Working Hypothesis

The most likely cause is **blocking inside the MQTT publish path**, not the spill task.

Why this is the leading hypothesis:

1. The spill path is already notification-driven, so the old `50 ms + 50 ms` polling tax is gone.
2. The baseline `avg ~20-30 ms` is consistent with the improved architecture.
3. The UART bursts align very closely with publisher stalls:
   - with about `4 telemetry records/s`
   - `Flushed 6` suggests about `1.5 s` of backlog accumulation
   - `Flushed 8` suggests about `2.0 s`
   - `Flushed 10` suggests about `2.5 s`
   - this matches observed `max ~1.0-2.5 s`
4. `mqtt_gw_task` still performs synchronous `cy_mqtt_publish()` calls, so a short broker/Wi-Fi/library stall directly blocks the only publisher task.

Under the current single-publisher design, the likely sequence is:

1. one or more `cy_mqtt_publish()` calls stall for hundreds of ms or seconds
2. `buffer_task` continues putting fresh records into `buffer_mgr`
3. the publisher resumes and flushes the accumulated backlog
4. `read_to_publish_*` spikes because some records waited in the buffer

## Why This Is Plausible But Still Unsatisfactory

This behavior is **possible and explainable** within the current architecture.

However, it is still a real quality problem because:

- tail latency becomes unpredictable
- the single-publisher path suffers from head-of-line blocking
- the dashboard is not showing a minor artifact; it is reflecting real delay episodes

So this is not interpreted as a broken metric. It is interpreted as a real runtime stall pattern.

## Reliability Status Caveat

The architecture is much improved, but the system is still not "lossless under all overload conditions".

Remaining loss paths still exist:

- telemetry can still be lost if both telemetry queue and emergency ring saturate
- `status_queue` still has no rescue ring
- `event_queue` still drops on full
- `buffer_mgr` still ultimately depends on bounded memory policy (`drop_oldest` / `drop_newest`)

This does not invalidate the persistence work, but it means overall data reliability should still be described conservatively.

## Mitigation Options Under Consideration

### Option A. Lower MQTT operation timeout

Current config:

- `MQTT_TIMEOUT_MS = 5000` in `rtos_test/configs/mqtt_client_config.h`

Possible change:

- reduce to `1000-1500 ms`

Expected effect:

- lower worst-case blocking time in `cy_mqtt_publish()`
- lower `max`
- fewer large backlog bursts

Tradeoff:

- more aggressive publish failures / reconnects on weak links

### Option B. Lower flush batch size

Current config:

- `flush_batch_size = 50` in `rtos_test/source/profiles/profile_default.h`

Possible change:

- reduce to something like `8-16`

Expected effect:

- lower head-of-line blocking in steady state
- lower `p95` and `max`

Tradeoff:

- slower backlog drain after reconnect / outage

### Option C. Dynamic flush batch

Idea:

- use a small batch in steady state
- use a larger batch only during explicit backlog recovery

This is considered one of the best practical follow-ups because it may reduce latency without sacrificing outage recovery too much.

### Option D. Better instrumentation first

Add debug logging around slow publishes in `mqtt_gw_task.c`, for example:

- log any publish taking `>200 ms`
- include duration and topic suffix (`/telemetry`, `/status`, `/metrics`)

This would directly confirm whether the long delays are in:

- telemetry publish
- metrics publish
- or reconnect/failure path

### Option E. Change timing metrics semantics

For analysis quality, another possible improvement is to switch latency stats from:

- rolling 100-sample ring

to:

- per metrics-publish window statistics

This would make root-cause diagnosis easier, though it would change dashboard semantics.

## Data That Would Most Help External Review

For one affected run, the most useful artifacts would be:

1. Dashboard CSV covering the same time interval for:
   - `read_to_publish_avg/p95/max`
   - `mqtt_publish_avg/max`
   - `pmbus_txn_avg/max`
   - `buffer_depth_ram`
   - `buffer_depth_flash`
   - `mqtt_pub_fail`
   - `mqtt_reconnects`
   - `buffer_enqueued`
   - `buffer_dequeued`
2. UART log from the same interval, especially:
   - `Flushed ... buffered records`
   - `Publish failed ...`
   - `Disconnected ...`
   - `Connection lost ...`
   - `Connected to broker`

These would allow much more confident separation between:

- MQTT-side stalls
- backlog/head-of-line effects
- PMBus-side timing issues

## Questions for External Reviewer

1. Does the current diagnosis that the dominant problem is **MQTT publish stall -> backlog accumulation -> elevated `read_to_publish_*`** seem correct?
2. Given the current architecture, which mitigation is best justified next:
   - lower MQTT timeout
   - lower or dynamic flush batch
   - deeper instrumentation first
3. Would you keep latency metrics as rolling 100-sample stats, or switch them to per-window stats for better diagnosis?
4. Would you treat the current overall system state as:
   - architecturally sound but with tail-latency issues
   - or still requiring another architectural change in the publish path?

## Current Bottom-Line Assessment

At this point the system appears to be:

- architecturally much stronger than before
- functionally correct on QSPI bring-up and persistence
- materially improved in queue evacuation and same-boot latency accounting
- still affected by intermittent publish-side stalls that produce large tail-latency spikes

The most likely remaining bottleneck is the synchronous single-publisher MQTT path, not the spill task or the persistence layer.
