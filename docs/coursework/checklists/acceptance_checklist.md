# Acceptance Checklist (Coursework Readiness)

## Structure

- [ ] All 13 mandatory sections are present.
- [ ] Technical assignment is included and mapped to implementation.
- [ ] Conclusions explicitly confirm requirement coverage.

## Requirement traceability

- [ ] Initialization requirement -> `main.c` trace exists.
- [ ] Driver requirement -> `pmbus_master.c` trace exists.
- [ ] Main algorithm requirement -> `pmbus_poll_task.c` + `mqtt_gw_task.c` trace exists.
- [ ] Testing requirement -> `Makefile` + tests + logs trace exists.

## Testing evidence

- [ ] Host tests pass with zero failures.
- [ ] Test result evidence file is attached.
- [ ] Log inventory and charts are attached.

## Graphics

- [ ] Functional scheme exported to PNG and PDF (A3).
- [ ] Principle wiring scheme exported to PNG and PDF (A3).
- [ ] UML component and sequence diagrams exported.
- [ ] Flowcharts for polling, publish/flush, reconnect/backoff exported.

## Defense package

- [ ] Explanatory note file prepared.
- [ ] Source code folder included.
- [ ] Graphic artifacts included.
- [ ] File/folder names are Latin letters and digits only.
