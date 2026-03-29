# T-3 - HIL Broker Outage on Default Profile

## Objective

Validate story `T-3` from the backlog by running the gateway on the `default`
profile, forcing a 60-second MQTT broker outage, and verifying that:

1. `queue_drops == 0` during the outage
2. records are buffered during the outage
3. buffered records flush after reconnect
4. no manual gateway reset is needed

---

## Scope

This procedure is for the `default` gateway timings and buffer settings:

| Parameter | Value |
|-----------|-------|
| Profile | `default` |
| Targets | 2 (`0x58`, `0x59`) |
| Poll period | 500 ms per target |
| Metrics period | 10000 ms |
| RAM buffer | 256 records |
| Flash buffer | disabled |
| Drop oldest | true |

Reference: [profile_default.h](../../rtos_test/source/profiles/profile_default.h)

---

## Important note about broker control

The repository `default` profile currently points to `broker.hivemq.com`, which
you cannot stop on demand. For a reproducible `T-3` HIL run, point the
`default` profile to a broker that you control on the same LAN, while keeping
all other `default` settings unchanged.

Recommended setup:

- run Mosquitto on the Windows PC
- set `mqtt.host` in `profile_default.h` to the PC's LAN IP address
- keep device table, polling periods, queue settings, topics, and JSON schema
  unchanged

---

## Important note about observability

For this repository's current capture setup, `T-3` must be judged from a
**combined artifact set** (`UART + metrics.jsonl + events.jsonl`), not from
`events.jsonl` alone.

Why:

- `capture.py` is itself an MQTT client on the same broker and only resubscribes
  after its own reconnect completes. A short-lived buffered event can therefore
  be missed even if the gateway published it correctly.
- `metrics.jsonl` is published only while MQTT is online, and metrics are not
  buffered. A transient `buffer_depth_ram > 0` during the outage may therefore
  be invisible in the saved metrics windows even when buffering did happen.
- The broker LWT payload `GATEWAY_UNEXPECTED_DISCONNECT` may appear on the
  `/events` topic, but it is broker-generated and should be treated as
  supplemental evidence, not as a replacement for the gateway event
  `MQTT_DISCONNECTED`.

Because of those constraints, this runbook treats:

- `queue_drops`, `buffer_enqueued`, `buffer_dequeued`, `buffer_dropped` from
  `metrics.jsonl` as the primary buffering evidence
- UART lines as the primary disconnect/reconnect evidence
- `events.jsonl` as supporting evidence when the capture client manages to
  observe the transient event messages

Example of a validated run using this combined-artifact method:

- [T-3a selective block results (2026-03-29)](notes/t3a_selective_block_results_2026-03-29.md)

---

## Equipment

| Role | Item |
|------|------|
| Gateway | CY8CKIT-062S2-43012 |
| Target A | PSC3 target at `0x58` |
| Target B | PSC3 target at `0x59` |
| Broker / capture host | Windows PC running Mosquitto and `capture.py` |

---

## Prerequisites

Before the run:

- `default` profile is flashed on the gateway
- both PMBus targets are powered and online
- Mosquitto is installed locally or available via Docker
- Python environment can run `scripts/capture/capture.py`
- gateway UART console is open

---

## Timeline

```text
T=0:00   Start broker
T=0:00   Start capture
T=0:00   Boot gateway
T=0:30   Verify normal MQTT flow
T=1:00   Stop broker
T=2:00   Restart broker
T=3:00   Stop capture
```

This gives:

- 60 s warm-up
- 60 s outage window
- 60 s post-recovery window

---

## Procedure

### 1. Start the broker

Native Mosquitto on Windows:

```powershell
cd E:\mtb_workspace\thesis_proj\scripts\mqtt_broker
mosquitto -c mosquitto.conf
```

Docker alternative:

```powershell
cd E:\mtb_workspace\thesis_proj\scripts\mqtt_broker
docker compose up -d
```

### 2. Start MQTT capture

```powershell
cd E:\mtb_workspace\thesis_proj\scripts\capture
python capture.py --host <broker-host> --gw thesis_gw01 --duration 180 --out-dir ..\logs\t3_default
```

Expected outputs:

- `telemetry.jsonl`
- `status.jsonl`
- `metrics.jsonl`
- `events.jsonl`

### 3. Boot the gateway and verify warm-up

Watch the UART console and confirm:

- boot banner shows `profile=default`
- both targets come online
- gateway prints `[MQTT] Connected to broker`

Let the system run for about 60 seconds.

### 4. Stop the broker for 60 seconds

Native Mosquitto:

```powershell
taskkill /IM mosquitto.exe /F
```

Docker:

```powershell
cd E:\mtb_workspace\thesis_proj\scripts\mqtt_broker
docker compose stop
```

During this window, the gateway should:

- log a disconnect on UART (`Disconnected (callback)` and
  `Connection lost — reconnecting...`)
- keep polling PMBus devices
- drain IPC queues into the RAM buffer

### 5. Restart the broker after 60 seconds

Native Mosquitto:

```powershell
cd E:\mtb_workspace\thesis_proj\scripts\mqtt_broker
mosquitto -c mosquitto.conf
```

Docker:

```powershell
cd E:\mtb_workspace\thesis_proj\scripts\mqtt_broker
docker compose start
```

During recovery, the gateway should:

- reconnect automatically
- log a successful reconnect on UART and usually post `MQTT_CONNECTED`
- flush buffered records

### 6. Let capture finish

Keep the system running until the 180-second capture completes.

---

## Pass / fail criteria

The run is a PASS if all conditions below are true:

1. `queue_drops == 0` in every `metrics.jsonl` window
2. `sum(counters_delta.buffer_enqueued) > 0` for the run
3. `sum(counters_delta.buffer_dequeued) > 0` after reconnect
4. `sum(counters_delta.buffer_dropped) == 0` for a 60-second outage
5. UART shows the disconnect/reconnect path:
   `Disconnected (callback)` and `Connected to broker`
6. gateway does not require a manual reset

Supporting evidence that strengthens the PASS decision, but is not mandatory in
this capture setup:

- `events.jsonl` contains `MQTT_DISCONNECTED`
- `events.jsonl` contains `MQTT_CONNECTED`
- `gauges.buffer_depth_ram` rises above zero in at least one saved metrics
  window
- `events.jsonl` contains the broker LWT payload `GATEWAY_UNEXPECTED_DISCONNECT`

The run is a FAIL if any of these happen:

- `queue_drops` becomes non-zero
- `sum(buffer_enqueued) == 0`
- buffered records never flush after reconnect
- `buffer_dropped` becomes non-zero for the 60 s outage
- gateway hangs or needs reboot

---

## Quick verification commands

### Metrics summary

```powershell
@'
import json, pathlib
p = pathlib.Path(r'E:\mtb_workspace\thesis_proj\scripts\logs\t3_default\metrics.jsonl')
rows = [json.loads(x) for x in p.open(encoding="utf-8")]
print("max queue_drops delta =", max(r["counters_delta"]["queue_drops"] for r in rows))
print("max buffer_depth_ram  =", max(r["gauges"]["buffer_depth_ram"] for r in rows))
print("sum buffer_enqueued   =", sum(r["counters_delta"]["buffer_enqueued"] for r in rows))
print("sum buffer_dequeued   =", sum(r["counters_delta"]["buffer_dequeued"] for r in rows))
print("sum buffer_dropped    =", sum(r["counters_delta"]["buffer_dropped"] for r in rows))
'@ | python -
```

### Event summary

```powershell
@'
import json, pathlib
p = pathlib.Path(r'E:\mtb_workspace\thesis_proj\scripts\logs\t3_default\events.jsonl')
types = [json.loads(x)["type"] for x in p.open(encoding="utf-8")]
print("MQTT_DISCONNECTED =", types.count("MQTT_DISCONNECTED"))
print("MQTT_CONNECTED    =", types.count("MQTT_CONNECTED"))
print("GATEWAY_UNEXPECTED_DISCONNECT =", types.count("GATEWAY_UNEXPECTED_DISCONNECT"))
'@ | python -
```

### Manual interpretation

Expected good result:

- `max queue_drops delta = 0`
- `sum buffer_enqueued > 0`
- `sum buffer_dequeued > 0`
- `sum buffer_dropped = 0` for a 60 s outage
- UART clearly shows disconnect and reconnect without manual reset

Nice to have, but not required for PASS in this setup:

- `max buffer_depth_ram > 0`
- at least one `MQTT_DISCONNECTED`
- at least one `MQTT_CONNECTED`
- `GATEWAY_UNEXPECTED_DISCONNECT` from broker LWT

---

## Evidence to save

Store these artifacts for the run:

- UART console log or screenshots
- `scripts/logs/t3_default/telemetry.jsonl`
- `scripts/logs/t3_default/status.jsonl`
- `scripts/logs/t3_default/metrics.jsonl`
- `scripts/logs/t3_default/events.jsonl`
- optional plots derived from `metrics.jsonl`

---

## Troubleshooting

If the run fails:

- check that the gateway really uses the `default` profile at boot
- verify that the broker host is reachable before the outage
- verify that both PMBus targets are online before starting the capture
- if `queue_drops` is non-zero, note the exact outage duration and save the log
- if `buffer_dropped` is non-zero, confirm that the outage did not exceed the
  practical RAM buffer capacity for the active traffic rate
