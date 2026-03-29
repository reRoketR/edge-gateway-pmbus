# T-2 Host I2C Recovery Results

## Verdict

This run is a **PASS for `T-2` host-side I2C recovery state tests**.

The host test implementation and `make test` output confirm that:

- `PMBUS_ERR_TIMEOUT` routes to the controller-reset path
- `PMBUS_ERR_NOT_READY` is included in the controller-reset routing logic
- `PMBUS_ERR_BUS_FAULT` routes to the SCL-based bus-recovery path
- distinct events and metrics are produced for successful recovery paths
- `recovery_settle_ms` is applied after successful recovery
- the full host test suite still passes

## Scope

Story reference:

- `T-2` in `C:\Users\Volodymyr\Downloads\story_backlog.md`

Procedure reference:

- [host_i2c_recovery.md](/E:/mtb_workspace/thesis_proj/docs/experiments/host_i2c_recovery.md#L1)

Primary test artifacts:

- [test_i2c_recovery.c](/E:/mtb_workspace/thesis_proj/rtos_test/tests/test_i2c_recovery.c#L1)
- [i2c_mock.c](/E:/mtb_workspace/thesis_proj/rtos_test/tests/stubs/i2c_mock.c#L1)
- [Makefile](/E:/mtb_workspace/thesis_proj/rtos_test/Makefile#L222)

## What was run

From [rtos_test](/E:/mtb_workspace/thesis_proj/rtos_test):

```powershell
make test
.\test_i2c_recovery.exe
```

Observed result:

- `test_i2c_recovery`: `51 passed, 0 failed`
- full `make test`: all host-side tests passed

## Coverage against `T-2` acceptance

### 1. Timeout routes to controller reset

In [test_i2c_recovery.c](/E:/mtb_workspace/thesis_proj/rtos_test/tests/test_i2c_recovery.c#L149):

- `PMBUS_ERR_TIMEOUT` is asserted to select the controller-reset path

In [test_i2c_recovery.c](/E:/mtb_workspace/thesis_proj/rtos_test/tests/test_i2c_recovery.c#L179):

- successful controller reset is verified end-to-end
- expected event: `EVT_I2C_CONTROLLER_RESET`
- expected metric: `i2c_controller_resets`

Console evidence from the run:

- `--- test_controller_reset_success ---`
- `WARN: resetting SCB after timeout`

### 2. Bus fault routes to SCL-based bus recovery

In [test_i2c_recovery.c](/E:/mtb_workspace/thesis_proj/rtos_test/tests/test_i2c_recovery.c#L149):

- `PMBUS_ERR_BUS_FAULT` is asserted to select the bus-recovery path

In [test_i2c_recovery.c](/E:/mtb_workspace/thesis_proj/rtos_test/tests/test_i2c_recovery.c#L227):

- successful bus recovery is verified end-to-end
- expected event: `EVT_PMBUS_BUS_RECOVERY`
- expected metric: `i2c_bus_recoveries`

Console evidence from the run:

- `--- test_bus_recovery_success ---`
- `Bus recovery OK`

### 3. `recovery_settle_ms` is applied

The configured settle delay in the test config is:

- `recovery_settle_ms = 5` in [test_i2c_recovery.c](/E:/mtb_workspace/thesis_proj/rtos_test/tests/test_i2c_recovery.c#L66)

This is asserted after successful controller reset:

- `i2c_mock_last_delay_ticks() == 5`
- `i2c_mock_total_delay_ticks() == 5`

and after successful bus recovery:

- `i2c_mock_last_delay_ticks() == 5`
- `i2c_mock_total_delay_ticks() == 5`

## Additional useful coverage

The current host test also verifies:

- `PMBUS_ERR_NOT_READY` is routed to controller reset
- `PMBUS_ERR_NACK` selects neither recovery path
- controller reset is skipped when the bus is not idle
- failed bus recovery posts `EVT_PMBUS_BUS_RECOVERY_FAIL`
- no success metric is incremented on failed recovery

This goes beyond the minimum `T-2` acceptance and is useful supporting evidence
for `D1-2` and `D1-3`.

## Full host-suite result

The full host suite passed from [rtos_test](/E:/mtb_workspace/thesis_proj/rtos_test):

- `test_buffer_ring`
- `test_pmbus_decode`
- `test_json_encode`
- `test_i2c_recovery`

Observed final line:

- `All host-side tests passed.`

## Conclusion

`T-2` can be treated as **DONE / PASS** for the current backlog interpretation.

It is a host-side verification story, so it does not replace HIL evidence, but
it does satisfy the intended unit/integration acceptance for:

- routing of recovery paths
- event emission
- metric increments
- settle-delay behavior
