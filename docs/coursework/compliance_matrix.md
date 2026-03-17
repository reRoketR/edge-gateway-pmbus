# Compliance Matrix (Methodical Guidelines vs Repository)

## Summary

Firmware implementation is present; report-level and graphic artifacts need to
be prepared before final defense.

| Guideline Requirement | Available in Project | Status | Required Action |
|---|---|---|---|
| Microcomputer initialization program | `rtos_test/source/main.c:72-122` | Available | Describe in Section 3 and include key listing in appendices |
| Peripheral driver | `rtos_test/source/pmbus_master.c:339`, `rtos_test/source/pmbus_master.c:453` | Available | Present as I2C/SMBus PMBus driver |
| Main processing algorithm | `rtos_test/source/pmbus_poll_task.c:93`, `rtos_test/source/mqtt_gw_task.c` | Available | Add flowcharts for polling/publish/reconnect |
| Software architecture | `rtos_test/README.md`, `rtos_test/source/gateway_ipc.c`, `docs/architecture.md` | Partial | Add UML component + sequence/class diagrams |
| Testing methodology | `rtos_test/Makefile:205`, `rtos_test/tests/*`, `rtos_test/logs/*`, `docs/mqtt_topics.md`, `docs/persistent_buffer.md`, `docs/pmbus_command_map.md` | Partial | Formalize test scenarios and pass/fail criteria |
| Graphic part (functional/principle/UML/algorithms) | No final prepared coursework graphics | Missing | Prepare and export all required diagrams |
| Explanatory note by template | Technical docs exist, no full coursework note | Missing | Prepare full report with all mandatory sections |

## Section-to-Source Mapping

### Section 2 (Architecture)

- `docs/architecture.md`
- `rtos_test/source/gateway_ipc.c`
- `rtos_test/source/pmbus_poll_task.c`
- `rtos_test/source/mqtt_gw_task.c`

### Section 3 (Software Development)

- `docs/mqtt_topics.md`
- `docs/pmbus_command_map.md`
- `rtos_test/source/main.c`
- `rtos_test/source/pmbus_master.c`
- `rtos_test/source/pmbus_poll_task.c`
- `rtos_test/source/mqtt_gw_task.c`

### Section 4 (Testing Methodology)

- `docs/persistent_buffer.md`
- `rtos_test/Makefile`
- `rtos_test/tests/`
- `rtos_test/logs/`
