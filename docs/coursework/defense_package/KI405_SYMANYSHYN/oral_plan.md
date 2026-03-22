# Oral Defense Plan

## 1. Topic and Objective

- Briefly present the PMBus-MQTT gateway purpose.
- State the coursework objective and the target laboratory use case.

## 2. Functional Scheme

- Show the role of `CY8CKIT-062S2-43012` as the gateway controller.
- Explain the external interfaces: `I2C/SMBus`, `Wi-Fi`, `MQTT`, `UART`.
- Point to the functional electrical scheme and the target PMBus device.

## 3. Software Architecture

- Explain task decomposition: `pmbus_poll_task`, `mqtt_gw_task`, `buffer_task`.
- Show IPC queues and the telemetry/status/event/metrics flow.
- Highlight the store-and-forward behavior during connectivity loss.

## 4. PMBus Driver and Recovery

- Explain `timeout`, `retry`, and `PEC` handling in the PMBus master driver.
- Describe the implemented SCB reset path and shared-bus backoff.
- Clarify what recovery limitations remain and why they are acceptable for coursework scope.

## 5. Testing and Evidence

- Show host-side test results with `0 failed`.
- Show runtime log inventory and representative charts.
- Explain the acceptance criteria and the achieved metrics.

## 6. Final Results

- Summarize what requirements are covered.
- Mention the assembled defense package contents.
- Close with current limitations and future work for the bachelor's stage.
