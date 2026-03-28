/* Stub cycfg_pins.h for host-side unit tests. */
#ifndef CYCFG_PINS_STUB_H
#define CYCFG_PINS_STUB_H

#include <stdint.h>

typedef uint32_t en_hsiom_sel_t;

#define HSIOM_SEL_GPIO 0u

#define CYBSP_I2C_SCL_PORT ((void *)0x11)
#define CYBSP_I2C_SCL_PIN  0u
#define CYBSP_I2C_SDA_PORT ((void *)0x22)
#define CYBSP_I2C_SDA_PIN  1u

#endif /* CYCFG_PINS_STUB_H */
