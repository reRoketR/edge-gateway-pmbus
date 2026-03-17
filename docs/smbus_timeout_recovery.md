# SMBus SCL-Low Timeout Recovery

## Status

Planned future work. Not required for the current coursework scope.

## Motivation

The current gateway uses a software transaction timeout and an optional
`9 x SCL` bus recovery sequence in [`pmbus_master.c`](/E:/mtb_workspace/thesis_proj/rtos_test/source/pmbus_master.c).
This is acceptable for the coursework baseline, but it is not the most
standards-aligned way to recover from a stuck SMBus/PMBus transaction,
especially during hot-plug or other line fault scenarios.

For SMBus/PMBus devices, a prolonged `SCL low` condition is already a
bus-level timeout mechanism. After the timeout window expires, devices are
expected to abort the current transaction and return to an idle state after a
short recovery interval. Because of this, a timeout-driven recovery path is a
better primary strategy than immediately injecting manual clock pulses.

## Proposed approach

Use a hardware timer to monitor the `SCL` line with minimal CPU overhead:

1. Route `SCL` to a timer trigger, capture, or equivalent external input.
2. Configure the timer so that it counts while `SCL` is held low.
3. Reset or reload the timer whenever `SCL` returns high.
4. If the low interval reaches the configured threshold, raise an interrupt.

In firmware, the interrupt should not be treated as the reset itself. The bus
reset is caused by the prolonged `SCL low` condition. The ISR is responsible
for synchronizing the gateway with that event:

1. Mark the active transaction as failed.
2. Abort the local SCB I2C transfer.
3. Reinitialize or re-enable the local I2C master state as needed.
4. Wait for the recovery interval before starting a new transaction.
5. Verify that `SCL` and `SDA` are both released before resuming polling.

## Optional active recovery mode

The same mechanism can be used proactively:

1. Temporarily take control of `SCL`.
2. Hold `SCL low` long enough to provoke an SMBus/PMBus timeout.
3. Release `SCL`.
4. Wait for the recovery interval.
5. Re-check bus idle state and resume communication.

This is conceptually cleaner for an SMBus/PMBus-only bus than the current
`9 x SCL` pulse method because it relies on timeout semantics already defined
by the protocol family.

## Expected benefits

- Better alignment with SMBus/PMBus timeout semantics.
- Near-zero CPU cost in the normal case.
- Cleaner recovery behavior during hot-plug or transient line faults.
- Less invasive than synthetic clock injection in mixed-master situations.

## Risks and limits

- This is still a bus-wide action: all devices on the bus are affected.
- It assumes SMBus/PMBus-compliant targets; generic I2C devices may behave
  differently.
- It does not fix hard shorts or permanently stuck lines by itself.
- It requires an available timer and practical signal routing on the selected
  MCU/pin configuration.
- It adds implementation and validation work that is disproportionate to the
  current coursework scope.

## Recommended project scope decision

For the coursework, keep the current software timeout plus diagnostic logging
and treat this feature as deferred future work.

For the bachelor thesis, consider this feature if one of the following becomes
an explicit objective:

- robust hot-plug handling on the PMBus/SMBus line;
- stronger recovery from line-level faults without manual reset;
- cleaner behavior in advanced bus-sharing scenarios.
