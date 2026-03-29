# T-6 QSPI Blackout Results: Persistent Backlog Survives Reboot

## Verdict

This run is a **PASS for the core T-6 objective**:

- the gateway booted with the QSPI persistent backend enabled
- a buffered backlog survived gateway reboot during broker outage
- the recovered backlog was flushed automatically after reconnect
- no corruption was detected in the recovered replay path

At the same time, this run is **not a clean no-loss blackout run**:

- upstream telemetry queue overflow occurred before reboot
- some records were dropped before they ever reached the persistent tier

For reporting, this run should be described as:

- `PASS for T-6 QSPI power-cycle recovery`
- `not evidence of lossless blackout buffering under prolonged outage`

## Context

Artifact directory:

- `scripts/logs/t6_blackout`

Method used:

- run gateway with `BUFFER_BACKEND=QSPI`
- induce broker outage
- allow backlog to accumulate
- hard reboot the gateway while outage is still active
- restore broker and observe recovery + flush

## What happened

### First boot: broker outage and backlog growth

UART from the run shows:

- MQTT initially connects
- publish failures force MQTT offline
- reconnect attempts begin
- telemetry queue pressure appears:
  - `telemetry queue and emergency ring full`
  - `status queue full`

The same overload is visible in `events.jsonl`:

- lines 3 through 8 are `QUEUE_OVERFLOW` with `detail="telemetry_queue"`

### Second boot: QSPI recovery

UART from the rebooted gateway shows:

- `Recovered seq=236, count=235 (Active Sec=0)`
- `Persistent backend: QSPI (S25FL512S)`
- successful broker reconnect
- flush batches:
  - `50`
  - `50`
  - `50`
  - `50`
  - `50`
  - `29`

This is direct evidence that a non-empty persistent backlog survived the reboot
and was replayed after reconnect.

## Primary evidence

### 1. Recovered backlog size matches post-reboot flush accounting

In `metrics.jsonl` line 3:

- `buffer_dequeued = 279`
- `buffer_enqueued = 44`
- `buffer_dropped = 0`
- `buffer_depth_flash = 0`

Interpretation:

- `279 - 44 = 235`
- this exactly matches the UART recovery message `count=235`
- therefore the post-reboot run flushed `235` recovered persistent records plus
  `44` newly buffered records created during the reboot window

This is strong evidence that the recovered QSPI backlog was actually consumed
and emptied.

### 2. Replay ordering is visible in buffered status traffic

In `status.jsonl`:

- lines 5 and 6 are post-reboot live status records at `ts_ms ≈ 1774807392...`
- lines 7 through 22 then contain older pre-reboot records with timestamps
  `1774807290...` through `1774807360...`

Interpretation:

- after reconnect, the gateway first emits new live traffic from the fresh boot
- then replays older buffered records from before the reboot
- this confirms that historical data survived and was published after recovery

### 3. The system returns to empty-buffer steady state after flush

In `metrics.jsonl` line 3:

- `buffer_depth_ram = 0`
- `buffer_depth_flash = 0`
- `telemetry_queue_depth = 0`

In `metrics.jsonl` line 4:

- steady-state publishing resumes with:
  - `buffer_dequeued = 0`
  - `buffer_depth_flash = 0`
  - `queue_drops = 0`

Interpretation:

- recovered data was not left stranded in flash
- after replay, the system returned to normal empty-buffer operation

## Why this is not a no-loss run

This run does **not** prove that the entire blackout was lossless.

Evidence of loss before persistence:

- `events.jsonl` lines 3 through 8: repeated `QUEUE_OVERFLOW`
- UART during outage:
  - `telemetry queue and emergency ring full`
  - `status queue full`

Interpretation:

- records were lost in the producer-side queues before they could be evacuated
  into the RAM/QSPI buffering tiers
- therefore this run proves **recovery of recoverable backlog**, not recovery of
  every generated record

## Note on `queue_drops = 0`

The metrics topic does not contradict the overflow evidence.

Reason:

- the queue-overflow warnings happened on the first boot during outage
- the gateway then rebooted before another metrics message from that boot was
  successfully published
- metrics counters restart from zero after reboot

So the overflow evidence for this run comes primarily from:

- UART warnings
- `events.jsonl` queue-overflow events

not from the post-reboot metrics window alone.

## Conclusion

This run is valid evidence for `T-6` because it demonstrates:

- QSPI persistent tier active at runtime
- non-zero backlog recovery after reboot
- successful flush of recovered records after broker reconnect
- return to empty-buffer steady state

The correct interpretation is:

- `PASS for QSPI backlog persistence across reboot`
- `not a clean lossless blackout-buffering result`

## Recommendation

Keep this note as the primary evidence for `T-6`.

If a second, stronger follow-up run is desired, run a shorter outage or enlarge
the upstream queueing stages so that:

- records spill to QSPI
- `Recovered ... count > 0` still appears after reboot
- but `QUEUE_OVERFLOW` never occurs

That would provide a cleaner demonstration of both persistence and no-loss
behavior under the chosen outage profile.
