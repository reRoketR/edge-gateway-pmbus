# T-4 - HIL Target Hot-Plug

## Objective

Validate story `T-4` from the backlog by power-cycling one PMBus target while
the gateway is running and proving that:

1. `PMBUS_DEVICE_OFFLINE` is posted after the offline threshold is reached
2. `PMBUS_DEVICE_ONLINE` is posted after the target returns
3. the gateway continues polling and does not hang or require a reset
4. recovery is visible in metrics and/or recovery events

---

## Scope

This procedure assumes the current `default` profile:

| Parameter | Value |
|-----------|-------|
| Profile | `default` |
| Targets | 2 (`0x58`, `0x59`) |
| Poll period | 500 ms per target |
| Offline fail threshold | 3 consecutive failed telemetry cycles |
| Expected offline detection window | about `3 x 500 ms = 1500 ms` |

References:

- `OFFLINE_FAIL_THRESHOLD` in `pmbus_poll_task.c`
- `profile_default.h`

Use Target B (`0x59`) as the hot-plugged device and keep Target A (`0x58`)
powered the whole time.

---

## Equipment

| Role | Item |
|------|------|
| Gateway | CY8CKIT-062S2-43012 |
| Stable target | PSC3 target at `0x58` |
| Hot-plug target | PSC3 target at `0x59` |
| Broker / capture host | Windows PC running Mosquitto and `capture.py` |

---

## Prerequisites

Before the run:

- gateway is flashed with the `default` profile
- both PMBus targets are initially online
- broker is reachable and capture is working
- UART console is open on the gateway
- you can safely remove and restore power to Target B only

If you already prepared the controlled local broker setup for `T-3`, reuse it.

---

## Timeline

```text
T=0:00   Start broker
T=0:00   Start capture
T=0:00   Boot gateway
T=0:30   Verify both targets online
T=1:00   Power OFF Target B (0x59)
T=1:10   Power ON Target B (0x59)
T=2:30   Stop capture
```

This gives:

- 60 s warm-up
- 10 s target outage
- 80 s post-recovery window

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

### 2. Start capture

```powershell
cd E:\mtb_workspace\thesis_proj\scripts\capture
python capture.py --host <broker-host> --gw thesis_gw01 --duration 150 --out-dir ..\logs\t4_hotplug
```

### 3. Boot gateway and verify steady state

Confirm on UART:

- `profile=default`
- Target A (`0x58`) is online
- Target B (`0x59`) is online
- MQTT is connected

Let the system run for about 60 seconds.

### 4. Power-cycle Target B

At about `T=1:00`, remove power from Target B only.

Keep Target B powered off for about 10 seconds.

At about `T=1:10`, restore power to Target B.

Important:

- do not reset the gateway
- do not disconnect Target A
- prefer true target power-cycle over random SDA/SCL wire disturbance

### 5. Let the gateway recover

Keep the run going until the 150-second capture completes.

During this time, the gateway should:

- keep publishing data for Target A
- detect Target B as offline
- later mark Target B online again
- continue operating without a manual reset

---

## Pass / fail criteria

The run is a PASS if all conditions below are true:

1. `PMBUS_DEVICE_OFFLINE` is present for `0x59`
2. `PMBUS_DEVICE_ONLINE` is later present for `0x59`
3. offline detection occurs within about 1500 ms to 2000 ms of Target B power-off
4. gateway never requires a manual reset
5. Target A telemetry continues throughout the disturbance
6. any recovery counters / events are consistent with the observed disturbance

The run is a FAIL if any of these happen:

- no offline event is posted
- no online event is posted after Target B returns
- gateway locks up, stops polling, or needs a reboot
- Target A also disappears unexpectedly for a prolonged period

---

## Quick verification commands

### Event summary

```powershell
@'
import json, pathlib
p = pathlib.Path(r'E:\mtb_workspace\thesis_proj\scripts\logs\t4_hotplug\events.jsonl')
rows = [json.loads(x) for x in p.open(encoding="utf-8")]
for evt in rows:
    if evt.get("type") in ("PMBUS_DEVICE_OFFLINE", "PMBUS_DEVICE_ONLINE",
                           "PMBUS_BUS_RECOVERY", "PMBUS_BUS_RECOVERY_FAILED",
                           "I2C_CONTROLLER_RESET"):
        print(evt.get("ts_ms"), evt.get("type"), evt.get("detail"))
'@ | python -
```

### Recovery metrics summary

```powershell
@'
import json, pathlib
p = pathlib.Path(r'E:\mtb_workspace\thesis_proj\scripts\logs\t4_hotplug\metrics.jsonl')
rows = [json.loads(x) for x in p.open(encoding="utf-8")]
print("sum pmbus_reads_fail      =", sum(r["counters_delta"].get("pmbus_reads_fail", 0) for r in rows))
print("sum pmbus_retries         =", sum(r["counters_delta"].get("pmbus_retries", 0) for r in rows))
print("sum i2c_controller_resets =", sum(r["counters_delta"].get("i2c_controller_resets", 0) for r in rows))
print("sum i2c_bus_recoveries    =", sum(r["counters_delta"].get("i2c_bus_recoveries", 0) for r in rows))
'@ | python -
```

### Manual interpretation

Expected good result:

- exactly one offline transition and one online transition for the hot-plugged target
- Target A remains active
- some failure / retry / recovery counters may increment during the disturbance
- no permanent stuck-bus behavior after Target B returns

---

## Evidence to save

Store:

- UART console log or screenshots
- `scripts/logs/t4_hotplug/telemetry.jsonl`
- `scripts/logs/t4_hotplug/status.jsonl`
- `scripts/logs/t4_hotplug/metrics.jsonl`
- `scripts/logs/t4_hotplug/events.jsonl`
- notes with approximate unplug and replug timestamps

---

## Troubleshooting

If the run fails:

- verify that Target B was truly power-cycled, not merely disconnected from one signal
- confirm that the gateway booted with the expected `default` profile
- confirm that Target A stayed powered and connected
- if no offline event appears, check whether the target outage was long enough to cross the
  `OFFLINE_FAIL_THRESHOLD`
- if the gateway hangs, preserve UART logs and recovery event / metrics evidence for analysis

