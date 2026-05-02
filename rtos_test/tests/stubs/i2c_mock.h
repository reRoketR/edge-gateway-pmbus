/* Shared state and helper accessors for host-side I2C recovery tests. */
#ifndef I2C_MOCK_H
#define I2C_MOCK_H

#include <stdint.h>
#include <stdbool.h>
#include "FreeRTOS.h"
#include "cy_scb_i2c.h"
#include "cycfg_pins.h"

void i2c_mock_reset(void);
void i2c_mock_set_master_status(uint32_t status);
void i2c_mock_set_write_result(cy_en_scb_i2c_status_t status);
void i2c_mock_set_read_result(cy_en_scb_i2c_status_t status);
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

/* ARA test helpers (D2c-1) */
void     i2c_mock_set_read_data(const uint8_t *data, uint8_t len);
uint8_t  i2c_mock_last_read_slave_addr(void);
uint8_t  i2c_mock_last_write_slave_addr(void);
bool     i2c_mock_read_was_called(void);
bool     i2c_mock_i2c_write_was_called_since_reset(void);
uint32_t i2c_mock_i2c_write_call_count(void);
uint32_t i2c_mock_i2c_read_call_count(void);
uint32_t i2c_mock_last_write_len(void);
uint32_t i2c_mock_last_read_len(void);
bool     i2c_mock_last_write_xfer_pending(void);
bool     i2c_mock_last_read_xfer_pending(void);
uint8_t  i2c_mock_last_write_byte(uint32_t index);

#endif /* I2C_MOCK_H */
