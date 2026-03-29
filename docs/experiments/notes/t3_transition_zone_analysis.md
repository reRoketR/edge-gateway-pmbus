# Analysis: T-3 MQTT Outage Transition Zone

## Context

We investigated `T-3` resilience behavior for the PMBus-MQTT edge gateway during MQTT outage conditions. The goal was to verify that the gateway transitions cleanly to store-and-forward mode without dropping telemetry during broker/connectivity loss.

Relevant files:
- `rtos_test/source/mqtt_gw_task.c`
- `rtos_test/source/pmbus_poll_task.c`
- `docs/experiments/hil_broker_outage.md`

## Two outage scenarios

### 1. Broker down / restart

The broker process is stopped or restarted. This is a real broker-side failure, but it also disconnects the observer (`capture.py`) because the observer uses the same broker.

Effect:
- good for testing broker failure recovery
- poor observability for transient metrics/events around reconnect

### 2. Selective gateway block

The broker remains alive, but only the gateway loses its path to the broker. This simulates a network blackhole from the gateway's perspective.

Effect:
- better observability because `capture.py` stays connected
- closer to real gateway-side connectivity loss, such as Wi-Fi/router/path failure

## What we observed

Under the selective-block scenario, the gateway behavior splits into four phases:

1. `Normal`: MQTT connected, publishing succeeds.
2. `Transition A`: `cy_mqtt_publish()` starts failing with `CY_RSLT_MODULE_MQTT_PUBLISH_FAIL (0x806000B)`, but the task still considers MQTT online.
3. `Transition B`: publish calls fail with `CY_RSLT_MODULE_MQTT_NOT_CONNECTED (0x806000E)`, but the disconnect callback still has not been processed by the main task loop.
4. `Offline/reconnect`: `Disconnected (callback)` is handled, MQTT is marked offline, queues are drained to the buffer, and reconnect/flush begins.

## Root cause

The vulnerability is in the transition zone before the disconnect callback is processed.

During that window:
- `mqtt_gw_task` still uses the online publish path
- synchronous `cy_mqtt_publish()` calls block and fail
- `pmbus_poll_task` continues enqueueing telemetry
- telemetry queue pressure rises
- this can result in `telemetry queue full` warnings and `queue_drops`

The buffering logic itself is not the problem. The problem is delayed failover from "online publish mode" to "offline drain-to-buffer mode".

## Observability caveat

When the broker is killed completely, `capture.py` can miss exactly the critical reconnect window because it disconnects too. That means:

- `events.jsonl` may miss `MQTT_DISCONNECTED`
- `metrics.jsonl` may miss the one snapshot where `queue_drops > 0`
- UART can show queue saturation while the saved metrics appear clean

This is an observability limitation of the test setup, not necessarily a contradiction in firmware behavior.

## Minimal fix

The recommended minimal fix is an early failover latch in `mqtt_gw_task.c`:

- if `cy_mqtt_publish()` returns a fatal connectivity-related error such as `PUBLISH_FAIL`, `NOT_CONNECTED`, or `CLOSED`
- latch a pending disconnect in the task
- stop further online queue processing in the current loop iteration
- let the main loop perform the normal centralized offline transition:
  - mark MQTT offline
  - post `EVT_MQTT_DISCONNECTED`
  - drain queues to the persistent buffer
  - enter reconnect/backoff logic

This avoids duplicating state transitions inside the publish helper while still shortening the vulnerable transition zone.

## Experiment methodology conclusion

For resilience validation, the selective gateway block should be treated as the primary T-3 scenario because it:

- keeps the observer alive
- exposes the real transition-zone weakness
- better matches realistic gateway-side network degradation

For clarity in documentation, the scenarios can be separated as:

- `T-3a`: gateway-to-broker connectivity loss (selective block / blackhole)
- `T-3b`: broker restart / broker down

## Open question for review

Other reviewers should evaluate:

1. whether the root cause is correctly identified as delayed failover before disconnect callback handling
2. whether the early-failover latch is the right minimal fix
3. whether `T-3a` and `T-3b` should be separated formally in the thesis and backlog evidence
