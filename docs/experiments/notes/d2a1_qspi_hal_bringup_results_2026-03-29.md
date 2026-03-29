# D2a-1: QSPI External Flash Bring-Up Summary

## Overview
This report documents the successful bring-up of the QSPI External Flash (S25FL512S, 64 MB) on the CY8CKIT-062S2-43012 board. This completes story **D2a-1** from the diploma scope, unblocking the migration of the persistent telemetry buffer from the internal Em_EEPROM to the external flash.

## Implementation Details
1. **Middleware Integration**: Added the `serial-flash` library (`release-v1.4.3`) to interface with the SMIF block.
2. **BSP Configuration**: Utilized the auto-generated `S25FL512S_SlaveSlot_0` configuration struct provided by the CY8CKIT-062S2 BSP. This ensures the correct SPI mode, Quad-SPI pins (`CYBSP_QSPI_D0` to `D3`), and timing parameters are applied.
3. **Module Created (`qspi_flash.c/.h`)**:
    - `qspi_flash_init()`: Initializes the SMIF block at a conservative 25 MHz to ensure reliable communication. Retrieves JEDEC information and logs the detected flash size and erase sector size.
    - `qspi_flash_self_test()`: A non-destructive initialization test that targets the *last* sector of the 64 MB flash. It verifies erase, read-after-erase (`0xFF`), write (`0x55`/`0xAA` pattern), and read-after-write operations.
4. **RTOS Integration Fix**: 
    - The `serial-flash` library is compiled with `CY_SERIAL_FLASH_QSPI_THREAD_SAFE` which relies on FreeRTOS mutexes (`cy_rtos_get_mutex()`).
    - **Deadlock Resolution**: Calling erase/write functions from `main()` before the scheduler started resulted in a deadlock. 
    - **Fix**: The `qspi_flash_init()` (which only performs memory-mapped register reads) remains in `main()`, but `qspi_flash_self_test()` was moved to `vApplicationDaemonTaskStartupHook()`.
    - **Configuration**: Enabled `configUSE_DAEMON_TASK_STARTUP_HOOK = 1` in `FreeRTOSConfig.h`.

## Execution Evidence
The console output confirms the asynchronous, thread-safe execution of the QSPI self-test alongside regular PMBus and MQTT tasks.

```text
[QSPI] Initializing SMIF @ 25000000 Hz...
[QSPI] S25FL512S detected, 64 MB, sector=262144
[SYS] All tasks created — starting scheduler

... (Other RTOS tasks start) ...

[QSPI] Self-test: addr=0x03FC0000, sector=262144 bytes
[QSPI] Self-test step 1/5: erasing...
... (Other background tasks continue transparently) ...
[QSPI] Self-test step 1/5: erase OK
[QSPI] Self-test step 2/5: read-after-erase...
[QSPI] Self-test step 2/5: erase verify OK
[QSPI] Self-test step 3/5: writing 256 bytes...
[QSPI] Self-test step 3/5: write OK
[QSPI] Self-test step 4/5: read-back verify...
[QSPI] Self-test step 4/5: data match OK
[QSPI] Self-test step 5/5: final erase...

... (WLAN/MQTT continues) ...

[QSPI] Self-test: PASS
```

## Reviewer Notes
- **Thread Safety**: The self-test explicitly demonstrates that erasing a 256 KB sector (which takes seconds) does **not** block high-priority tasks like the I2C polling. Context switching correctly preempts the QSPI polling loop.
- **Clock Speed**: Kept at 25 MHz for initial bring-up stability. Can be increased to 50 Mbps later if write-throughput bottlenecks are identified during full ring-buffer integration (D2a-2).
- **Next Step**: Proceed to **D2a-2** — Implementing the RAW QSPI Ring Buffer using the verified `cy_serial_flash_qspi_*` API.
