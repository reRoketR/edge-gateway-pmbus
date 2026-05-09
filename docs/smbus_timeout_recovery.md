# SMBus Timeout and Bus Recovery

## Status

Implemented baseline recovery is present in `pmbus_master.c` and covered by
host-side tests. A dedicated hardware `SCL low` monitor is not implemented; it
remains an optional enhancement for stronger hot-plug and line-fault handling.

## Current Implementation

The gateway uses a layered recovery strategy for PMBus/SMBus transactions:

1. Each transfer is guarded by `transaction_timeout_ms` from `g_config.i2c`.
2. On timeout, the pending SCB I2C read/write transfer is aborted.
3. If the SCB master remains busy after the abort settle window, the firmware
   resets the SCB controller only when both `SCL` and `SDA` are released.
4. If the bus is not idle, the firmware skips the controller reset and arms a
   shared-bus backoff window so polling does not immediately charge repeated
   failures to every configured target.
5. For bus-fault cases, optional bus recovery toggles `SCL` nine times and
   checks whether `SDA` was released.
6. After successful recovery, `recovery_settle_ms` is applied before the next
   transaction.

The implementation reports distinct events and metrics for the recovery paths:

- `I2C_CONTROLLER_RESET`
- `PMBUS_BUS_RECOVERY`
- `PMBUS_BUS_RECOVERY_FAILED`
- `i2c_controller_resets`
- `i2c_bus_recoveries`

The behavior is validated by `test_i2c_recovery`, which checks routing of
`TIMEOUT` / `NOT_READY` to the controller-reset path, routing of `BUS_FAULT` to
the `SCL` recovery path, idle-line checks, shared-bus backoff, events, metrics,
and settle-delay handling.

## Current Limits

The current firmware does not continuously measure how long `SCL` is held low
with a hardware timer. It detects timeout conditions from the SCB transfer path
and then synchronizes the firmware state with the bus state. This is adequate
for the current gateway experiments, but it is not a complete hardware-level
SMBus timeout detector.

The recovery path also cannot fix hard electrical faults by itself:

- a permanent short to ground on `SCL` or `SDA`;
- a target that never releases the bus;
- incorrect pull-up or wiring conditions;
- non-SMBus I2C devices with incompatible timeout behavior.

## Optional Hardware SCL-Low Monitor

For a stricter SMBus/PMBus-only deployment, a future hardware monitor can be
added:

1. Route `SCL` to a timer trigger, capture input, or equivalent GPIO interrupt
   path.
2. Count only while `SCL` is held low.
3. Reset or reload the timer whenever `SCL` returns high.
4. Raise an interrupt when the configured low-time threshold is reached.
5. In the ISR or deferred handler, mark the active transaction as failed, abort
   the local SCB transfer, reinitialize the master state if needed, wait for
   the recovery interval, and verify that `SCL` and `SDA` are both released.

This enhancement would reduce CPU involvement in the normal case and align the
implementation more closely with SMBus timeout semantics during hot-plug or
line-fault scenarios. It should be treated as a robustness extension, not as a
missing requirement for the current thesis implementation.

## Project Scope Decision

For the current thesis firmware, keep the implemented software timeout,
controller reset, shared-bus backoff, and optional `9 x SCL` recovery path.
Mention the hardware `SCL low` monitor only as a possible improvement for
industrial-grade hot-plug and bus-sharing scenarios.
