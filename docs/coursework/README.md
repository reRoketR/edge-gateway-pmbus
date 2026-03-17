# Coursework Package: PMBus-MQTT Gateway

This folder contains a ready-to-fill coursework package based on the methodical
guidelines and the current implementation in `rtos_test/`.

## Scope

- Theme fixed: **PMBus-MQTT gateway for telemetry collection and transfer**
- No firmware API refactoring required
- Focus on reporting artifacts and defense package

## Structure

- `compliance_matrix.md` - requirement-to-artifact mapping
- `report/coursework_report_draft.md` - full explanatory note draft skeleton
- `report/technical_assignment.md` - 1-page technical assignment template
- `report/references.md` - source list draft
- `checklists/` - formatting, acceptance, and defense checklists
- `evidence/` - test and log evidence snapshots
- `diagrams/src/` - source diagrams in `.drawio` and `.puml`
- `diagrams/exports/` - export targets (`.png`, `.pdf` A3)
- `defense_package/` - electronic submission package spec

## Diagram Source Policy

Use two source formats:

- **PlantUML** for UML and flow diagrams
- **diagrams.net (draw.io)** for wiring and electrical diagrams

Source and export policy:

1. Author UML and flow diagrams in `.puml` (`diagrams/src/`)
2. Author wiring/electrical diagrams in `.drawio` (`diagrams/src/`)
3. Export for explanatory note to `.png`
4. Export for graphic part to `.pdf` (A3)

## Expected Workflow

1. Fill report sections in `report/coursework_report_draft.md`.
2. Edit `.puml` for UML/flow figures and `.drawio` for wiring/electrical figures.
3. Export figures to `diagrams/exports/`.
4. Insert exported figures into DOCX/PDF explanatory note.
5. Assemble final package according to `defense_package/README.md`.
