# Defense Package Manifest

Package folder: `KI405_SYMANYSHYN`

Note:

- Folder name uses the existing coursework template.
- Rename the folder if the official submission requires a different group or surname spelling.
- `simplified_principle_wiring` is intentionally omitted from the mandatory graphics set.

## Files

| Item | Path | Present |
|---|---|---|
| Explanatory note DOCX | `report/coursework_report.docx` | [x] |
| Explanatory note PDF | `report/coursework_report.pdf` | [x] |
| Source code | `source/rtos_test/` | [x] |
| Functional scheme PDF | `graphics/exports/functional_electrical_scheme.pdf` | [x] |
| UML component PDF | `graphics/exports/uml_component.pdf` | [x] |
| UML sequence PDF | `graphics/exports/uml_sequence_pmbus_to_mqtt.pdf` | [x] |
| Init flowchart PDF | `graphics/exports/flow_init_main_to_scheduler.pdf` | [x] |
| Polling flowchart PDF | `graphics/exports/flow_polling_loop.pdf` | [x] |
| Publish/flush flowchart PDF | `graphics/exports/flow_publish_flush.pdf` | [x] |
| Reconnect flowchart PDF | `graphics/exports/flow_reconnect_backoff.pdf` | [x] |
| Diagram source files | `graphics/source/` | [x] |
| Test evidence | `evidence/` | [x] |
| IDE/debug screenshot | `evidence/screenshots/vscode_modustoolbox_debug.png` | [x] |
| Oral explanation plan | `oral_plan.md` | [x] |

## Reference Commands

Build:

```text
cd rtos_test
make getlibs
make build TOOLCHAIN=GCC_ARM CONFIG=Debug
```

Host tests:

```text
cd rtos_test
make test
```

## Verification

- [x] All package file and folder names use Latin letters, digits, and underscore.
- [x] Final report files are included.
- [x] Exported graphics are included.
- [x] Source project is included.
- [x] Testing evidence and screenshots are included.
