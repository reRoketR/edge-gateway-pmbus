/* Stub cy_serial_flash_qspi.h for host-side QSPI buffer tests. */
#ifndef CY_SERIAL_FLASH_QSPI_STUB_H
#define CY_SERIAL_FLASH_QSPI_STUB_H

#include <stddef.h>
#include <stdint.h>
#include "cy_result.h"

cy_rslt_t cy_serial_flash_qspi_write(uint32_t addr, size_t length, const uint8_t *data);
cy_rslt_t cy_serial_flash_qspi_read(uint32_t addr, size_t length, uint8_t *data);
cy_rslt_t cy_serial_flash_qspi_erase(uint32_t addr, size_t length);

#endif /* CY_SERIAL_FLASH_QSPI_STUB_H */
