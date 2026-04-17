# T-7 - HIL SMBALERT / ARA Verification

## Objective

Validate stories `D2c-1` (gateway SMBALERT detect + ARA + urgent status poll)
and `D2c-2` (target SMBALERT trigger) by running both boards with a wired
SMBALERT# line, injecting faults via UART, and verifying the end-to-end
alert→ARA→status pipeline.

---

## Scope

| Parameter | Value |
|-----------|-------|
| Profile | `default` |
| Target board | KIT_PSC3M5_EVK (I2C addr 0x58) |
| Gateway board | CY8CKIT-062S2-43012 |
| SMBALERT# pin | CYBSP_D7 on both boards |
| External pull-up | 4.7 kΩ to 3.3 V on D7 line |
| Poll period | 5000 ms (for clear separation) |
| Status period | 5000 ms |

---

## Hardware Setup

1. Connect **D7 ↔ D7** between both boards (open-drain, wired-AND).
2. Add **4.7 kΩ** pull-up resistor from the D7 line to 3.3 V.
3. Connect shared **GND** between both boards.
4. I2C bus (SCL/SDA) connected as per `docs/hw/wiring.md`.
5. Both boards powered via USB; UART terminals open on each.

Verify with a multimeter that D7 reads ~3.3 V when idle (no fault asserted).

---

## Firmware

- **Target**: build with `MTB_PMBUS_SUPPORT_SMBALERT = 1U` (already set).
- **Gateway**: build with `smbalert_enabled = true` in the default profile.
- Optionally set `poll_period_ms = 5000` and `status_period_ms = 5000` in the
  profile for clear timing separation.

---

## Procedure

### Step 1 — Baseline

1. Flash both boards.
2. Open UART terminals for both.
3. Wait for the gateway to print `[POLL] SMBALERT# GPIO ISR registered on CYBSP_D7`.
4. Confirm at least one normal status poll cycle completes (visible in UART or
   MQTT events).
5. Record gateway `metrics.jsonl`: `smbalert_count` should be 0.

### Step 2 — Assert SMBALERT

1. In the **target** UART terminal, press **`a`**.
2. Target should print:
   - `[FAULT] Latching fault — asserting SMBALERT#`
   - STATUS_WORD should become `0x0002` (next register update).
3. Measure D7 line with multimeter or logic analyser: should be LOW (~0 V).

### Step 3 — Verify gateway detection

1. Within the next poll tick (≤10 ms), the gateway should detect the falling
   edge and print:
   - `[POLL] SMBALERT from dev 0x58 → urgent status`
2. An **urgent** status poll should fire immediately (not waiting for the 5 s
   deadline).
3. Verify in MQTT / `events.jsonl`:
   - `EVT_SMBALERT_RECEIVED` event with `detail = 0x58`.
4. Verify in `metrics.jsonl`:
   - `smbalert_count` incremented by 1.
5. Verify that the status record published after the urgent poll shows
   STATUS_WORD `0x0002`.

### Step 4 — Clear fault

1. In the **target** UART terminal, press **`c`**.
2. Target should print:
   - `[FAULT] Clearing fault and SMBALERT#`
3. D7 line should return HIGH (~3.3 V).
4. Next periodic status poll should show STATUS_WORD `0x0000`.

### Step 5 — Repeat with ARA NACK verification

1. Press **`a`** on target, wait for gateway to handle ARA.
2. Immediately press **`a`** again (or let the ARA loop do a second read).
3. The gateway ARA loop should NACK on the second `pmbus_ara_read()` and
   print no error — just exit the ARA loop normally.
4. Verify no `bus_recovery_count` increment and no `retry_count` increment
   attributable to the ARA read.

### Step 6 — Bus backoff guard

1. Trigger a condition that activates bus backoff (e.g., multiple NACKs on a
   different device, or temporarily disconnect I2C).
2. Press **`a`** on target while backoff is active.
3. Gateway should **not** attempt ARA during backoff; `s_smbalert_pending`
   should be preserved and processed after backoff expires.
4. After backoff clears, verify ARA runs and urgent status fires.

---

## Pass Criteria

| Check | Expected |
|-------|----------|
| SMBALERT detection latency | < 10 ms from assertion to ISR (MVP) |
| ARA response address | 0x58 (matches configured target) |
| Urgent status fires before periodic deadline | Yes |
| STATUS_WORD in urgent poll | reflects latched fault (0x0002) |
| `smbalert_count` metric incremented | +1 per alert |
| `EVT_SMBALERT_RECEIVED` event posted | with detail = target addr |
| ARA NACK terminates loop cleanly | no error log, no counter pollution |
| Clear restores idle state | STATUS_WORD 0x0000, D7 HIGH |
| Bus backoff defers ARA | flag preserved, processed after backoff |

---

## Artifacts

- UART logs from both boards (copy-paste or screen capture)
- `events.jsonl` from capture script
- `metrics.jsonl` from capture script
- Logic analyser trace of D7 line (optional, for latency measurement)
