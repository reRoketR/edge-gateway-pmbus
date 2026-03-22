# Diagram Sources and Export Rules

UML and flow diagrams are authored in **PlantUML**.
Electrical diagrams remain in **diagrams.net (draw.io)**.

## Source files

Stored in:

- `src/*.puml` for UML and flow diagrams
- `src/*.drawio` for wiring and electrical diagrams

## Export targets

Store exports in:

- `exports/*.png` for insertion into explanatory note
- `exports/*.pdf` for graphic part submission (A3 where applicable)

## Required diagrams

1. `functional_electrical_scheme.drawio`
2. `uml_component.puml`
3. `uml_sequence_pmbus_to_mqtt.puml`
4. `flow_polling_loop.puml`
5. `flow_publish_flush.puml`
6. `flow_reconnect_backoff.puml`
7. `flow_init_main_to_scheduler.puml`

## Notes

- Existing `.drawio` files for UML/flow may be kept as legacy drafts, but
  `.puml` sources are the canonical artifacts for the coursework package.
- Export PlantUML figures to the filenames expected by the report and defense
  package, including alias names where required:
  `uml_component.*`, `uml_sequence_pmbus_to_mqtt.*`,
  `flow_polling_loop.*`, `flow_publish_flush.*`, `flow_reconnect_backoff.*`,
  `flow_init_main_to_scheduler.*`,
  `flow_pmbus_polling_task.*`, `flow_mqtt_publish_reconnect.*`,
  `flow_store_and_forward_buffer.*`.
