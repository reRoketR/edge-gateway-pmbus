# Bill of Materials (BOM)

Hardware components required to reproduce the PMBus↔MQTT Edge Gateway thesis experiments.

---

## 1 Evaluation Boards

| # | Part Number | Description | Qty | Role | Approx. Cost |
|---|-------------|-------------|-----|------|--------------|
| 1 | CY8CKIT-062S2-43012 | PSoC 62 + CYW43012 Wi-Fi/BT Pioneer Kit | 1 | Gateway (I2C master, Wi-Fi MQTT client) | ~$50 USD |
| 2 | KIT_PSC3M5_EVK | PSC3M5 Evaluation Kit | 1 | PMBus target (I2C slave, simulated PSU) | ~$40 USD |

---

## 2 Cables and Connectors

| # | Item | Description | Qty | Notes |
|---|------|-------------|-----|-------|
| 3 | USB Type-A to Micro-B cable | KitProg3 debug/power cable | 2 | One per board |
| 4 | Male-to-male jumper wires | Breadboard jumpers, ~10–15 cm | 3 | SCL, SDA, GND connections |

---

## 3 Passive Components (optional)

| # | Item | Value | Qty | Notes |
|---|------|-------|-----|-------|
| 5 | Pull-up resistor | 4.7 kΩ, ¼ W | 2 | Only if on-board pull-ups are insufficient |
| 6 | Bypass capacitor | 100 nF ceramic | 2 | Near each board's I2C pins (noise reduction) |
| 7 | Breadboard | Half-size solderless | 1 | Optional, for external pull-ups and clean wiring |

> **Note:** In most lab setups, the on-board pull-up resistors on both evaluation kits are sufficient. Items 5–7 are only needed if bus integrity issues are observed.

---

## 4 Host PC / Software

| # | Item | Version | Notes |
|---|------|---------|-------|
| 8 | PC or Laptop | Windows 10/11 | USB ports for both boards |
| 9 | ModusToolbox | 3.7 | IDE + make build system + libraries |
| 10 | Python | 3.9+ | For capture.py and plot.py scripts |
| 11 | Mosquitto MQTT broker | 2.x | Runs on host PC (or Docker) |
| 12 | Doxygen | 1.12.0 | Documentation generation (optional) |

### Python Packages

| Package | Version | Purpose |
|---------|---------|---------|
| paho-mqtt | 2.1.0+ | MQTT subscriber for data capture |
| matplotlib | 3.9+ | Plot generation |
| pandas | 2.3+ | Data analysis |

---

## 5 Network Infrastructure

| # | Item | Notes |
|---|------|-------|
| 13 | Wi-Fi access point (2.4 GHz, WPA2) | Gateway connects wirelessly |
| 14 | Ethernet or Wi-Fi for host PC | Host PC runs MQTT broker on LAN |

The gateway board and host PC must be on the same LAN subnet. Default broker address: `192.168.1.2:1883`.

---

## 6 Summary

| Category | Items | Est. Total Cost |
|----------|-------|-----------------|
| Evaluation boards | 2 | ~$90 USD |
| Cables | 2 USB + 3 jumper wires | ~$10 USD |
| Passives (optional) | Resistors, caps, breadboard | ~$5 USD |
| Software | All free/open-source | $0 |
| **Total** | | **~$105 USD** |

All components are commercially available from major distributors (Mouser, DigiKey, Farnell) or directly from Infineon.
