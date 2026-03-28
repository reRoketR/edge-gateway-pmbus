/* Stub cy_gpio.h for host-side unit tests. */
#ifndef CY_GPIO_STUB_H
#define CY_GPIO_STUB_H

#include <stdint.h>
#include "cycfg_pins.h"

#define CY_GPIO_DM_OD_DRIVESLOW 1u

uint32_t Cy_GPIO_Read(void *port, uint32_t pin);
en_hsiom_sel_t Cy_GPIO_GetHSIOM(void *port, uint32_t pin);
void Cy_GPIO_SetHSIOM(void *port, uint32_t pin, en_hsiom_sel_t hsiom);
void Cy_GPIO_SetDrivemode(void *port, uint32_t pin, uint32_t mode);
void Cy_GPIO_Write(void *port, uint32_t pin, uint32_t value);

#endif /* CY_GPIO_STUB_H */
