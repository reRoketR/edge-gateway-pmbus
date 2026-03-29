# T-4 XRES Results: Clean Single-Target Reset Case

## Verdict

This run is a **PASS for clean single-target unavailability and recovery**.

It is valid evidence that:

- holding `XRES` on one PMBus target makes only that target disappear
- the other target remains operational
- the gateway continues polling and publishing without reset
- the affected target later returns online cleanly

For reporting, this run should be described as:

- `PASS as T-4a clean target-reset / logical unavailability evidence`

It is also acceptable supporting evidence for `T-4` if the method is stated
honestly as `XRES reset`, not true USB power removal.

## Context

Artifact directory:

- `scripts/logs/t4_xres`

Method used:

- first hold `XRES` on target `0x58`
- later hold `XRES` on target `0x59`

The goal was to verify a clean single-device disturbance method after the USB
unplug experiment proved to be a shared-bus fault in this hardware setup.

## What happened

In `events.jsonl`:

- line 1: `PMBUS_DEVICE_OFFLINE` for `0x58`
- line 2: `PMBUS_DEVICE_ONLINE` for `0x58`
- line 3: `PMBUS_DEVICE_OFFLINE` for `0x59`
- line 4: `PMBUS_DEVICE_ONLINE` for `0x59`

Interpretation:

- the first XRES episode affected only `0x58`
- the second XRES episode affected only `0x59`
- there is no evidence of both targets dropping offline together

## Evidence that the healthy target stayed alive

From `status.jsonl` during the first XRES episode on `0x58`:

- lines 17 through 19 still contain normal status messages for `0x59`

From `status.jsonl` during the second XRES episode on `0x59`:

- lines 20 through 23 still contain normal status messages for `0x58`

From `telemetry.jsonl`:

- `0x58` telemetry is present from line 1 through line 636
- `0x59` telemetry is present from line 2 through line 637

This is strong evidence that the unaffected target remained active while the
other target was held in reset.

## Metrics evidence

The disturbance windows show target-local `NACK` failures, but no shared-bus
collapse.

### First XRES episode (`0x58`)

In `metrics.jsonl` line 9:

- `pmbus_reads_fail = 9`
- `pmbus_retries = 18`
- `pmbus_nack = 9`
- `i2c_controller_resets = 0`
- `queue_drops = 0`

In `metrics.jsonl` line 10:

- `pmbus_reads_fail = 6`
- `pmbus_retries = 12`
- `pmbus_nack = 6`
- `i2c_controller_resets = 0`
- `queue_drops = 0`

Recovery is already visible in `metrics.jsonl` line 11:

- `pmbus_reads_fail = 0`
- `pmbus_nack = 0`
- `queue_drops = 0`

### Second XRES episode (`0x59`)

In `metrics.jsonl` line 12:

- `pmbus_reads_fail = 7`
- `pmbus_retries = 14`
- `pmbus_nack = 7`
- `i2c_controller_resets = 0`
- `queue_drops = 0`

In `metrics.jsonl` line 13:

- `pmbus_reads_fail = 7`
- `pmbus_retries = 14`
- `pmbus_nack = 7`
- `i2c_controller_resets = 0`
- `queue_drops = 0`

Recovery is visible in `metrics.jsonl` line 14:

- `pmbus_reads_fail = 2`
- `pmbus_nack = 2`
- `queue_drops = 0`

And by later windows the run returns to steady-state values.

## Why this is cleaner than USB unplug

Unlike the USB unplug experiment:

- there are no mass `TIMEOUT` storms
- there are no `i2c_controller_resets`
- there is no evidence that both targets disappear together
- the disturbance is dominated by `NACK`, which matches a single target being
  reset or temporarily not answering

This means `XRES` is a clean target-local method in this setup, while USB power
removal is not.

## Conclusion

This run is suitable as the primary clean evidence for the target hot-plug
story if the method is described as:

- `single-target reset / logical unavailability via XRES`

It proves:

- offline detection for the affected target
- online detection after return
- uninterrupted gateway operation
- continued activity of the unaffected target
- no queue loss or bus-wide collapse during the disturbance

## Recommendation

For the thesis and defense package:

- keep the USB unplug result as a separate shared-bus stress case
- use this XRES run as the clean single-target `T-4a` evidence
- state explicitly that true power removal on this bench disturbs the shared bus,
  while `XRES` gives a target-local disturbance
