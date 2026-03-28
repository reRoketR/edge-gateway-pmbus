/* Shared state and helper accessors for host-side I2C recovery tests. */
#ifndef I2C_MOCK_H
#define I2C_MOCK_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "cycfg_pins.h"

void i2c_mock_reset(void);
void i2c_mock_set_master_status(uint32_t status);
void i2c_mock_set_scl_level(uint32_t level);
void i2c_mock_set_sda_level(uint32_t level);
void i2c_mock_set_scl_hsiom(en_hsiom_sel_t hsiom);

uint32_t i2c_mock_disable_calls(void);
uint32_t i2c_mock_enable_calls(void);
uint32_t i2c_mock_write_calls(void);
uint32_t i2c_mock_delay_us_calls(void);
TickType_t i2c_mock_last_delay_ticks(void);
TickType_t i2c_mock_total_delay_ticks(void);
TickType_t i2c_mock_tick_count(void);

#endif /* I2C_MOCK_H */
