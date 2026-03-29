# T-2 - Host-side I2C Recovery State Transition Tests

## Objective

Validate story `T-2` from the backlog by adding host-side unit tests for the
formalized recovery split in `pmbus_master.c` and proving that:

1. `PMBUS_ERR_TIMEOUT` and `PMBUS_ERR_NOT_READY` route to the controller-reset path
2. `PMBUS_ERR_BUS_FAULT` routes to the SCL-based bus-recovery path
3. distinct events and metrics are produced for each successful path
4. `recovery_settle_ms` is applied after each recovery path

Validated result note:

- [t2_host_i2c_recovery_results_2026-03-29.md](/E:/mtb_workspace/thesis_proj/docs/experiments/notes/t2_host_i2c_recovery_results_2026-03-29.md#L1)

---

## Scope

This plan covers the future host-side test pair:

| Planned file | Purpose |
|--------------|---------|
| `rtos_test/tests/test_i2c_recovery.c` | Recovery state transition tests |
| `rtos_test/tests/stubs/i2c_mock.c` | PDL I2C mock / call recorder |

It is intended to validate the current split implemented in:

- `should_attempt_controller_reset()`
- `should_attempt_bus_recovery()`
- `pmbus_reset_controller_if_idle()`
- `pmbus_bus_recovery()`

---

## Test harness design

The host test should stub or spy on:

- `Cy_SCB_I2C_*` driver calls used by `pmbus_master.c`
- `gateway_ipc_post_event()`
- `metrics_inc_i2c_controller_resets()`
- `metrics_inc_i2c_bus_recoveries()`
- `vTaskDelay()` or equivalent delay hook used for `recovery_settle_ms`

Recommended approach:

- expose static helpers through a small unit-test seam
- use deterministic spies that record:
  - which recovery function was called
  - which event type was posted
  - which metric increment function was called
  - whether settle delay was applied

---

## Test matrix

| ID | Scenario | Expected result |
|----|----------|-----------------|
| T2-1 | `PMBUS_ERR_TIMEOUT` | controller-reset path selected |
| T2-2 | `PMBUS_ERR_NOT_READY` | controller-reset path selected |
| T2-3 | `PMBUS_ERR_BUS_FAULT` | SCL-based bus-recovery path selected |
| T2-4 | `PMBUS_ERR_NACK` | neither recovery path selected |
| T2-5 | successful controller reset | `EVT_I2C_CONTROLLER_RESET` posted and `i2c_controller_resets` incremented |
| T2-6 | successful bus recovery | `EVT_PMBUS_BUS_RECOVERY` posted and `i2c_bus_recoveries` incremented |
| T2-7 | failed bus recovery | `EVT_PMBUS_BUS_RECOVERY_FAIL` posted and success metric not incremented |
| T2-8 | controller reset or bus recovery success | `recovery_settle_ms` delay applied |

---

## Procedure

### 1. Implement the test files

Add:

- `rtos_test/tests/test_i2c_recovery.c`
- `rtos_test/tests/stubs/i2c_mock.c`

The test should isolate the recovery logic and avoid any real hardware access.

### 2. Extend the host test target

Update the `test` target in `rtos_test/Makefile` so that it builds and runs the
new recovery test alongside the existing host tests.

Expected end-state:

```text
test_buffer_ring
test_pmbus_decode
test_json_encode
test_i2c_recovery
```

### 3. Run the host-side test suite

Primary path:

```powershell
cd E:\mtb_workspace\thesis_proj\rtos_test
make test
```

Windows fallback if the wrapper still uses Unix-style `./test_*` execution:

```powershell
cd E:\mtb_workspace\thesis_proj\rtos_test
gcc -Wall -Wextra -Werror -Isource -Isource/profiles -Itests/stubs `
    -o test_i2c_recovery.exe tests/test_i2c_recovery.c tests/stubs/i2c_mock.c `
    source/pmbus_master.c source/events.c source/metrics.c source/gateway_config.c `
    source/gw_util.c -lm
.\test_i2c_recovery.exe
```

Adjust the exact source list if the final test seam uses additional helpers.

---

## Assertions to check

For each scenario, verify all of the following:

- selected recovery path matches the input error status
- wrong recovery path is not invoked
- expected event type is posted exactly once
- expected success metric increments exactly once
- unrelated success metric stays unchanged
- `recovery_settle_ms` delay is applied after success

For negative-path cases:

- no success event is posted
- no success metric increments
- no settle delay is applied unless explicitly required by the design

---

## Pass / fail criteria

The run is a PASS if:

1. all `T2-*` scenarios pass
2. the new test is integrated into `make test`
3. existing host tests still pass
4. recovery routing, events, metrics, and settle-delay behavior all match the design

The run is a FAIL if:

- any scenario selects the wrong recovery path
- event / metric hooks are missing or duplicated
- `recovery_settle_ms` is not observed
- the test only verifies JSON strings and not the actual path logic

---

## Evidence to save

Store:

- `test_i2c_recovery.c`
- `i2c_mock.c`
- `make test` console output
- direct `test_i2c_recovery.exe` output if Windows fallback was used
- brief notes describing how the PDL calls were mocked

---

## Notes

- This is a host-side verification plan, not an HIL experiment.
- It complements, but does not replace, the hot-plug HIL validation from `T-4`.
- If the implementation keeps the historical event name `EVT_PMBUS_BUS_RECOVERY`
  instead of renaming it to `EVT_I2C_BUS_RECOVERY`, the test should follow the
  actual codebase contract consistently.
