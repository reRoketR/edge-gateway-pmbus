# T-3 - HIL Broker Outage on Default Profile

## Objective

Validate story `T-3` from the backlog by running the gateway on the `default`
profile, forcing a 60-second MQTT broker outage, and verifying that:

1. `queue_drops == 0` during the outage
2. `buffer_depth_ram` grows during the outage
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

- post `MQTT_DISCONNECTED`
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
- post `MQTT_CONNECTED`
- flush buffered records

### 6. Let capture finish

Keep the system running until the 180-second capture completes.

---

## Pass / fail criteria

The run is a PASS if all conditions below are true:

1. `queue_drops == 0` in every `metrics.jsonl` window
2. `gauges.buffer_depth_ram` rises above zero during the 60-second outage
3. `counters_delta.buffer_dequeued` becomes non-zero after reconnect
4. `gauges.buffer_depth_ram` returns to zero or near zero after flush
5. `events.jsonl` contains both `MQTT_DISCONNECTED` and `MQTT_CONNECTED`
6. gateway does not require a manual reset

The run is a FAIL if any of these happen:

- `queue_drops` becomes non-zero
- buffer never grows during outage
- buffered records never flush after reconnect
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
'@ | python -
```

### Manual interpretation

Expected good result:

- `max queue_drops delta = 0`
- `max buffer_depth_ram > 0`
- `sum buffer_enqueued > 0`
- `sum buffer_dequeued > 0`
- `sum buffer_dropped = 0` for a 60 s outage
- at least one `MQTT_DISCONNECTED` and one `MQTT_CONNECTED`

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

