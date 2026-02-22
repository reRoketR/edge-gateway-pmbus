# Hardware Wiring Guide

This document describes the physical wiring between the PMBus gateway and target boards used in the thesis experiments.

---

## 1 Overview

Two Infineon evaluation kits are connected via a 3-wire I²C/SMBus link.

```
                  ┌──────────────────────────────────────┐
                  │         CY8CKIT-062S2-43012          │
                  │        (Gateway / I2C Master)        │
                  │                                      │
                  │   J6-Pin  Signal   PSoC 62 Pin       │
                  │   ──────  ──────   ──────────        │
                  │   P6_0    SCL      SCB3.SCL          │
                  │   P6_1    SDA      SCB3.SDA          │
                  │   GND     GND      ─                 │
                  └──────────┬──┬──┬──────────────────────┘
                             │  │  │
                     SCL ────┘  │  └──── GND
                                │
                     SDA ───────┘
                             │  │  │
                  ┌──────────┴──┴──┴──────────────────────┐
                  │         KIT_PSC3M5_EVK                │
                  │        (Target / PMBus Slave)         │
                  │        Address: 0x58                  │
                  │                                      │
                  │   J?-Pin  Signal   PSC3 Pin           │
                  │   ──────  ──────   ──────────        │
                  │   P9_0    SCL      SCB0.SCL          │
                  │   P9_2    SDA      SCB0.SDA          │
                  │   GND     GND      ─                 │
                  └──────────────────────────────────────┘
```

---

## 2 Wiring Table

| Wire # | Signal | Gateway Board (CY8CKIT-062S2-43012) | Target Board (KIT_PSC3M5_EVK) | Color (suggested) |
|--------|--------|-------------------------------------|-------------------------------|--------------------|
| 1      | SCL    | J6 → P6_0                          | J? → P9_0                    | Yellow             |
| 2      | SDA    | J6 → P6_1                          | J? → P9_2                    | Green              |
| 3      | GND    | J6 → GND                           | J? → GND                     | Black              |

> **Note:** Both boards are powered independently via their own USB connections to the host PC (KitProg3 debugger port). No external power supply wiring is required.

---

## 3 Pull-up Resistors

SMBus/I²C requires pull-up resistors on both SCL and SDA lines.

| Parameter | Value | Notes |
|-----------|-------|-------|
| Pull-up resistance | 4.7 kΩ | Standard value for 100 kHz I²C |
| Pull-up voltage | 3.3 V | Both boards operate at 3.3 V I/O |

### 3.1 Pull-up Source

**Option A — On-board pull-ups (preferred):**
Both evaluation kits include internal/on-board pull-up resistors on their I²C pins. Verify that the board's solder jumpers or DIP switches for I²C pull-ups are enabled. In most cases, no external resistors are needed.

**Option B — External pull-ups:**
If on-board pull-ups are not available or need to be replaced:

```
        3.3 V            3.3 V
          │                 │
         ┌┤                ┌┤
         │ 4.7 kΩ          │ 4.7 kΩ
         └┤                └┤
          │                 │
          ├──── SCL         ├──── SDA
          │                 │
```

Solder or breadboard the resistors between the 3.3 V supply rail and each bus line.

---

## 4 Bus Configuration

| Parameter | Value |
|-----------|-------|
| Bus speed | 100 kHz (standard mode) |
| Protocol | SMBus / PMBus (subset of I²C) |
| Addressing | 7-bit, slave addr = 0x58 |
| PEC | CRC-8 enabled by default |
| Max wire length | < 30 cm recommended for breadboard/jumper wiring |

---

## 5 USB Connections

| Board | USB Port | Purpose |
|-------|----------|---------|
| CY8CKIT-062S2-43012 | KitProg3 USB (J6) | Debug UART (115200 baud) + programming + power |
| KIT_PSC3M5_EVK | KitProg3 USB | Debug UART + programming + power |

Both boards connect to the host PC via USB. The gateway board's debug UART is used for:
- Boot banner and runtime logging (115200 baud, 8N1)
- Programming via `make program`

---

## 6 Wi-Fi Connection

The CY8CKIT-062S2-43012 connects to the local Wi-Fi network via the on-board CYW43012 radio.

| Parameter | Value |
|-----------|-------|
| Band | 2.4 GHz |
| Security | WPA2-PSK (configured in `wifi_config.h`) |
| MQTT broker | 192.168.1.2:1883 (Mosquitto on host PC) |

---

## 7 Physical Setup Photo Reference

```
 ┌─────────────────────────────────────────────────────────────┐
 │                    Host PC / Laptop                         │
 │                                                             │
 │   ┌─────────────────┐          ┌─────────────────┐         │
 │   │  USB cable #1   │          │  USB cable #2   │         │
 │   └────────┬────────┘          └────────┬────────┘         │
 │            │                            │                   │
 │            │   Mosquitto Broker         │                   │
 │            │   (port 1883)              │                   │
 │            │   capture.py               │                   │
 │            │   plot.py                  │                   │
 └────────────┼────────────────────────────┼───────────────────┘
              │                            │
     ┌────────┴────────┐          ┌────────┴────────┐
     │  CY8CKIT-062S2  │  3-wire  │  KIT_PSC3M5_EVK │
     │  -43012         │◄────────►│                  │
     │  (Gateway)      │  I2C bus │  (PMBus Target)  │
     └─────────────────┘          └──────────────────┘
```

---

## 8 Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|--------------|-----|
| I2C NACK on all commands | Wrong slave address or wiring | Verify addr 0x58, check SDA/SCL not swapped |
| Intermittent CRC/PEC failures | Noisy bus or long wires | Shorten jumper wires, add 100 nF bypass caps |
| Gateway cannot ping broker | Wi-Fi not connected | Check SSID/password in `wifi_config.h` |
| No UART output | Wrong COM port or baud rate | Use 115200 baud, check KitProg3 COM port in Device Manager |
| Target firmware not responding | Target not programmed | Flash target with `make program` in target_proj |
