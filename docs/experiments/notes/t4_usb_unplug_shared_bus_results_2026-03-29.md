# T-4 USB Unplug Results: Shared-Bus Disturbance Case

## Verdict

This run is **not a clean PASS for T-4 hot-plug of a single target**.

It is valid evidence for a different scenario:

- **USB power removal of one PMBus target caused a shared-bus disturbance**
- both PMBus targets became temporarily unavailable
- the gateway recovered without manual reset

Therefore this run should be classified as:

- `FAIL as clean single-target hot-plug evidence`
- `PASS as shared-bus disturbance recovery evidence`

## Context

Artifact directory:

- `scripts/logs/t4_default`

Method used:

- first unplug one target from USB power
- later unplug the other target from USB power

The purpose was to observe whether removing power from one target behaves like a clean single-device disappearance or disturbs the whole shared PMBus/I2C bus.

## What happened

### First unplug sequence

In `events.jsonl`:

- `0x58` went offline at line 16
- `0x59` went offline at line 20
- `0x59` came back online at line 81
- `0x58` came back online at line 82

### Second unplug sequence

In `events.jsonl`:

- `0x59` went offline at line 94
- `0x58` went offline at line 98
- `0x58` came back online at line 123
- `0x59` came back online at line 124

Interpretation:

- unplugging one USB-powered target did **not** isolate the disturbance to that target
- both devices disappeared from the gateway's point of view
- the disturbance propagated through the shared PMBus/I2C bus

## Recovery behavior

The gateway recovered after both disturbance episodes:

- both targets returned online after the first unplug
- both targets returned online after the second unplug
- telemetry resumed for both targets by the end of the run

From `telemetry.jsonl`:

- `0x58` is present from the beginning to the end of the capture
- `0x59` is present from the beginning to the end of the capture

This confirms that the gateway did not require a manual reboot to resume operation.

## Metrics evidence

The metrics show severe bus disruption during the unplug windows.

### First disturbance window

In `metrics.jsonl` line 7:

- `pmbus_reads_fail = 20`
- `pmbus_retries = 40`
- `pmbus_timeouts = 20`
- `i2c_controller_resets = 60`

In `metrics.jsonl` line 8:

- `pmbus_reads_ok = 0`
- `pmbus_reads_fail = 9`
- `pmbus_retries = 18`
- `pmbus_timeouts = 6`
- `i2c_controller_resets = 17`

### Recovery after first disturbance

In `metrics.jsonl` lines 9 and 10:

- successful reads return
- controller resets drop back down
- PMBus timing gradually normalizes

### Second disturbance window

In `metrics.jsonl` line 11:

- `pmbus_reads_fail = 14`
- `pmbus_retries = 28`
- `pmbus_timeouts = 12`
- `i2c_controller_resets = 35`

In `metrics.jsonl` line 12:

- `pmbus_reads_fail = 2`
- `pmbus_retries = 5`
- `pmbus_nack = 1`
- `i2c_controller_resets = 3`

### Recovery after second disturbance

In `metrics.jsonl` lines 13 through 18:

- PMBus reads return to normal steady-state values
- `queue_drops` remains `0`
- no permanent degradation remains visible

## UART evidence

UART logs from the same run show:

- repeated PMBus `TIMEOUT`
- repeated `I2C_CONTROLLER_RESET`
- cases where the controller reset is skipped because the bus is not idle
- periods with `scl=0 sda=0`
- later return of both devices to `ONLINE`

This is strong evidence that the unplug operation caused a **shared electrical bus disturbance**, not a clean disappearance of only one target.

## Conclusion

This experiment should not be used as the primary evidence for story `T-4` if `T-4` is meant to prove:

- one target disappears
- the other target remains healthy
- gateway continues operating against the healthy target

It should instead be kept as an additional stress-case result proving:

- power removal of one USB-powered target can disturb the entire shared PMBus bus in this hardware setup
- the firmware does not hang permanently
- the gateway eventually recovers both devices without manual reset

## Recommendation

Use this note as evidence for a separate stress scenario such as:

- `shared-bus disturbance on target power removal`

For clean `T-4` evidence, run a second experiment using a method that does not electrically drag the shared bus, for example:

- holding `XRES` on a single target
- or another target-local reset/power mechanism that leaves the PMBus lines electrically stable
