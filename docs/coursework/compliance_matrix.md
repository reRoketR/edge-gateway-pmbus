# Compliance Matrix (Methodical Guidelines vs Repository)

## Summary

Firmware implementation, explanatory note, graphics, and defense package are
prepared in the repository. The remaining final step before submission is a
manual formatting audit of the final DOCX/PDF.

| Guideline Requirement | Available in Project | Status | Required Action |
|---|---|---|---|
| Microcomputer initialization program | `rtos_test/source/main.c`, `docs/coursework/report/sections/05_section3_software.md` | Available | Mapped in Section 3 and included in the final report |
| Peripheral driver | `rtos_test/source/pmbus_master.c`, `docs/coursework/report/sections/05_section3_software.md` | Available | Presented as the PMBus master driver in Section 3 |
| Main processing algorithm | `rtos_test/source/pmbus_poll_task.c`, `rtos_test/source/mqtt_gw_task.c`, exported flowcharts | Available | Covered by Sections 2-3 and exported algorithm diagrams |
| Software architecture | `rtos_test/source/gateway_ipc.c`, `docs/coursework/report/sections/04_section2_architecture.md`, UML exports | Available | UML component and sequence diagrams are prepared |
| Testing methodology | `rtos_test/Makefile`, `rtos_test/tests/*`, `rtos_test/logs/*`, `docs/coursework/evidence/*`, `docs/coursework/report/sections/06_section4_testing.md` | Available | Formalized in Section 4 with evidence files |
| Graphic part (functional/UML/algorithms) | `docs/coursework/diagrams/exports/*` | Available | Final exports are prepared; `simplified_principle_wiring` is intentionally excluded from the mandatory set |
| Explanatory note by template | `docs/coursework/report/coursework_report.docx`, `docs/coursework/report/coursework_report.pdf` | Available | Final explanatory note files are prepared |

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
