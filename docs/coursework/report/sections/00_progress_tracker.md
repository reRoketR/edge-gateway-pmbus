# Report Progress Tracker

| Section | File | Status | Notes |
|---|---|---|---|
| Technical Assignment | `01_technical_assignment.md` | Included in final report | Mapped to implementation and test criteria |
| Introduction | `02_introduction.md` | Included in final report | Scope, objective, tasks, object, and subject are present |
| Section 1: Functional Scheme | `03_section1_functional_scheme.md` | Included in final report | Functional scheme exported and referenced |
| Section 2: Architecture | `04_section2_architecture.md` | Included in final report | UML and core architecture flowcharts exported |
| Section 3: Software | `05_section3_software.md` | Included in final report | Source traceability to `main`, `pmbus_master`, `pmbus_poll`, `mqtt_gw` completed |
| Section 4: Testing | `06_section4_testing.md` | Included in final report | Host tests, runtime logs, and charts attached |
| Conclusions | `07_conclusions.md` | Included in final report | Requirement coverage, achieved metrics, and limitations recorded |

## Finalization

1. Perform the last manual formatting audit on the final DOCX/PDF.
2. Rename the defense package folder if the official group/surname spelling differs from the current template.
3. Optionally add a dedicated outage/recovery runtime session if it is needed for the oral defense.

## Deferred For Bachelor's

1. Plan SMBus timeout-driven recovery via a hardware timer on `SCL`: timeout monitor, ISR-based abort/re-init, and optional forced bus reset.
2. Keep this feature out of the mandatory coursework scope and return to it only if advanced hot-plug robustness is required later.

## Citation Policy

1. Use references `[n]` only for external literature, standards, and articles.
2. Treat internal `docs/*.md` files as project artifacts without `[n]`.
