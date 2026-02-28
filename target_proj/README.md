# PMBus Target Simulator  KIT_PSC3M5_EVK

Simulates a 48 V-in / 12 V-out power supply module on the PSOC Control C3
evaluation kit using the Infineon `mtb-pmbus` middleware.  Responds to PMBus
read commands from the gateway (master) with slowly-varying sinusoidal
telemetry values.

## Supported PMBus commands

| Command            | Code | Direction  | Format     |
|--------------------|------|------------|------------|
| CLEAR_FAULTS       | 0x03 | Write      | Send Byte  |
| VOUT_MODE          | 0x20 | Read       | 8-bit      |
| STATUS_WORD        | 0x79 | Read       | 16-bit     |
| STATUS_VOUT        | 0x7A | Read       | 8-bit      |
| STATUS_IOUT        | 0x7B | Read       | 8-bit      |
| STATUS_TEMPERATURE | 0x7D | Read       | 8-bit      |
| READ_VIN           | 0x88 | Read       | Linear11   |
| READ_IIN           | 0x89 | Read       | Linear11   |
| READ_VOUT          | 0x8B | Read       | Linear16   |
| READ_IOUT          | 0x8C | Read       | Linear11   |
| READ_TEMPERATURE_1 | 0x8D | Read       | Linear11   |
| READ_POUT          | 0x96 | Read       | Linear11   |

## Hardware setup

| Signal     | Pin       | Function                |
|------------|-----------|-------------------------|
| I2C SCL    | P9_0      | PMBus target clock      |
| I2C SDA    | P9_2      | PMBus target data       |
| Debug UART | P6_2/P6_3 | Retarget-IO console     |

Target address: **0x58**, PEC enabled.

Connect the I2C lines (SCL, SDA) between this kit and the gateway
(CY8CKIT-062S2-43012) using dupont wires.  Ensure a common ground.

## Requirements

- [ModusToolbox](https://www.infineon.com/modustoolbox) v3.3+
- BSP: KIT_PSC3M5_EVK v1.0.3+
- Toolchain: GCC_ARM (default)

## Build & program

```bash
make build TOOLCHAIN=GCC_ARM CONFIG=Debug -j$(nproc)
make program
```

## Related

- **Gateway project**: `../rtos_test/`  PMBus-MQTT Edge Gateway on CY8CKIT-062S2-43012
- **mtb-pmbus middleware**: `../mtb_shared/mtb-pmbus/`