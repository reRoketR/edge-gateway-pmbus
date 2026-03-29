# T-3a Results: Selective Gateway Block

## Verdict

`T-3a` passed under the selective gateway block method.

The gateway survived MQTT connectivity loss without manual reset, buffered telemetry during the outage, reconnected successfully, and flushed the buffered backlog after broker reachability was restored.

## Test setup

- Scenario: gateway-to-broker connectivity loss (`selective block` / network blackhole)
- Profile: `default`
- Broker: `192.168.1.6:1883`
- Gateway ID: `thesis_gw01`
- Firmware build shown on UART: `Mar 29 2026 12:35:24`
- Artifact directory: `scripts/logs/t3_default`

Reference runbook:
- `docs/experiments/hil_broker_outage.md`

Related analysis:
- `docs/experiments/notes/t3_transition_zone_analysis.md`

## Primary evidence

### Metrics window covering the outage and reconnect

In `scripts/logs/t3_default/metrics.jsonl`, the reconnect window shows:

- `mqtt_pub_fail = 3`
- `mqtt_reconnects = 1`
- `buffer_enqueued = 148`
- `buffer_dequeued = 50`
- `buffer_dropped = 0`
- `queue_drops = 0`
- `buffer_depth_ram = 98`

This appears in line 8 of the captured artifact.

Interpretation:
- telemetry was accepted into the offline buffer during the outage
- no buffered records were dropped
- no IPC queue drops were recorded
- a reconnect happened within the same window

### Metrics window after reconnect flush

In the following metrics window (line 9 of `metrics.jsonl`):

- `buffer_dequeued = 98`
- `buffer_depth_ram = 0`
- `queue_drops = 0`

Interpretation:
- the remaining buffered telemetry was flushed successfully
- RAM buffer returned to empty state after reconnect

## Supporting event evidence

In `scripts/logs/t3_default/events.jsonl`:

- line 1: `GATEWAY_UNEXPECTED_DISCONNECT` (broker LWT)
- line 3: `MQTT_DISCONNECTED` with `detail = "publish_fail"`
- line 2: `MQTT_CONNECTED`

Note:
- `MQTT_CONNECTED` was received before the buffered `MQTT_DISCONNECTED` record, which is expected in this setup because the disconnect event was buffered during the outage and published after reconnect.

## UART evidence

UART log from the same run shows:

- successful initial connect
- three consecutive `Publish failed (0x806000B)` messages
- `WARN: forcing offline after publish failure`
- `Disconnected (publish_fail)`
- `Connection lost — reconnecting...`
- successful reconnect to broker
- post-reconnect buffered flush:
  - `Flushed 50 buffered records`
  - `Flushed 50 buffered records`
  - `Flushed 48 buffered records`

Interpretation:
- the application-level early failover path triggered as designed
- store-and-forward remained operational after forced offline transition
- total buffered records flushed after reconnect matched the backlog observed in metrics

## Acceptance summary

The revised `T-3a` pass criteria are satisfied:

- `queue_drops == 0`: yes
- `sum(buffer_enqueued) > 0`: yes
- `sum(buffer_dequeued) > 0`: yes
- `sum(buffer_dropped) == 0`: yes
- reconnect without manual reset: yes

## Notes

- This result validates the selective gateway block scenario as a strong resilience test for the gateway.
- The run also confirms that the early-failover logic prevents the previous observability gap where the task stayed too long in the online publish path.
- Latency spikes remained visible during post-reconnect flush, but they do not invalidate the resilience result.
