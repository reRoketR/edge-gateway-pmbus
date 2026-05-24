# PMBus Gateway — Live Dashboard

Single-page web dashboard for real-time monitoring of the PMBus/MQTT gateway.

## Connection

The dashboard connects to an MQTT broker via **WebSocket** (Paho MQTT JS
client). Enter the broker host, WS port, and optional base topic in the
header bar, then click **Connect**.

## Known Limitations

The following limitations are intentional for the coursework scope and are
documented here so that a first-time reader understands the dashboard's
boundaries:

1. **WebSocket-only, no retained state on connect.**
   The dashboard subscribes to live MQTT topics over WebSocket. It does **not**
   request retained messages or query historical data on connect. If the
   dashboard is opened after the gateway has been running, only new messages
   from that point onward are displayed.

2. **No offline data replay.**
   When the gateway reconnects after a broker outage and flushes its
   store-and-forward buffer, those buffered records are published to
   `pmbus/<gw>/buffered/*` topics. The dashboard currently treats them the same
   as live data — it does not visually distinguish buffered-vs-live records or
   reconstruct the original timeline.

3. **Chart memory grows unbounded in long sessions.**
   The streaming charts (Chart.js + chartjs-plugin-streaming) append data
   points indefinitely. In sessions lasting several hours, browser memory usage
   will grow continuously. There is no automatic pruning or circular-buffer
   limit on the client side. Refresh the page to reclaim memory.

4. **No authentication or access control.**
   The dashboard connects to the broker anonymously. It relies on the broker's
   own access control (if any). This is acceptable for a local lab network.

## Tech Stack

| Library | Version | Purpose |
|---------|---------|---------|
| Chart.js | 4.4.3 | Canvas-based charts |
| Luxon | 3.4.4 | Time axis adapter |
| chartjs-plugin-streaming | 2.0.0 | Real-time scrolling |
| chartjs-plugin-zoom | 2.0.1 | Scroll-zoom / pan |
| Paho MQTT | 1.1.0 | WebSocket MQTT client |
