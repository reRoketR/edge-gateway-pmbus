#include "i2c_mock.h"

#include "cy_scb_i2c.h"
#include "cy_sysint.h"
#include "cy_sysclk.h"
#include "cycfg_peripherals.h"
#include "cycfg_clocks.h"
#include "cy_gpio.h"
#include "cy_syslib.h"

static uint32_t s_master_status;
static uint32_t s_scl_level;
static uint32_t s_sda_level;
static en_hsiom_sel_t s_scl_hsiom;
static uint32_t s_disable_calls;
static uint32_t s_enable_calls;
static uint32_t s_write_calls;
static uint32_t s_delay_us_calls;
static TickType_t s_tick_count;
static TickType_t s_last_delay_ticks;
static TickType_t s_total_delay_ticks;

int mock_pmbus_controller_hw = 0;
const int PMBUS_CONTROLLER_config = 0;

void i2c_mock_reset(void)
{
    s_master_status = 0u;
    s_scl_level = 1u;
    s_sda_level = 1u;
    s_scl_hsiom = 19u;
    s_disable_calls = 0u;
    s_enable_calls = 0u;
    s_write_calls = 0u;
    s_delay_us_calls = 0u;
    s_tick_count = 0u;
    s_last_delay_ticks = 0u;
    s_total_delay_ticks = 0u;
}

void i2c_mock_set_master_status(uint32_t status) { s_master_status = status; }
void i2c_mock_set_scl_level(uint32_t level) { s_scl_level = level; }
void i2c_mock_set_sda_level(uint32_t level) { s_sda_level = level; }
void i2c_mock_set_scl_hsiom(en_hsiom_sel_t hsiom) { s_scl_hsiom = hsiom; }

uint32_t i2c_mock_disable_calls(void) { return s_disable_calls; }
uint32_t i2c_mock_enable_calls(void) { return s_enable_calls; }
uint32_t i2c_mock_write_calls(void) { return s_write_calls; }
uint32_t i2c_mock_delay_us_calls(void) { return s_delay_us_calls; }
TickType_t i2c_mock_last_delay_ticks(void) { return s_last_delay_ticks; }
TickType_t i2c_mock_total_delay_ticks(void) { return s_total_delay_ticks; }
TickType_t i2c_mock_tick_count(void) { return s_tick_count; }

TickType_t xTaskGetTickCount(void)
{
    return s_tick_count;
}

void vTaskDelay(TickType_t ticks)
{
    s_last_delay_ticks = ticks;
    s_total_delay_ticks += ticks;
    s_tick_count += ticks;
}

cy_en_scb_i2c_status_t Cy_SCB_I2C_Init(void *base, const void *config,
                                       cy_stc_scb_i2c_context_t *context)
{
    (void)base;
    (void)config;
    (void)context;
    return CY_SCB_I2C_SUCCESS;
}

uint32_t Cy_SCB_I2C_SetDataRate(void *base, uint32_t dataRate, uint32_t scbClock)
{
    (void)base;
    (void)scbClock;
    return dataRate;
}

void Cy_SCB_I2C_Enable(void *base)
{
    (void)base;
    s_enable_calls++;
}

void Cy_SCB_I2C_Disable(void *base, cy_stc_scb_i2c_context_t *context)
{
    (void)base;
    (void)context;
    s_disable_calls++;
}

void Cy_SCB_I2C_Interrupt(void *base, cy_stc_scb_i2c_context_t *context)
{
    (void)base;
    (void)context;
}

uint32_t Cy_SCB_I2C_MasterGetStatus(void *base, cy_stc_scb_i2c_context_t *context)
{
    (void)base;
    (void)context;
    return s_master_status;
}

cy_en_scb_i2c_status_t Cy_SCB_I2C_MasterWrite(
    void *base,
    cy_stc_scb_i2c_master_xfer_config_t *config,
    cy_stc_scb_i2c_context_t *context)
{
    (void)base;
    (void)config;
    (void)context;
    return CY_SCB_I2C_SUCCESS;
}

cy_en_scb_i2c_status_t Cy_SCB_I2C_MasterRead(
    void *base,
    cy_stc_scb_i2c_master_xfer_config_t *config,
    cy_stc_scb_i2c_context_t *context)
{
    (void)base;
    (void)config;
    (void)context;
    return CY_SCB_I2C_SUCCESS;
}

void Cy_SCB_I2C_MasterAbortWrite(void *base, cy_stc_scb_i2c_context_t *context)
{
    (void)base;
    (void)context;
}

void Cy_SCB_I2C_MasterAbortRead(void *base, cy_stc_scb_i2c_context_t *context)
{
    (void)base;
    (void)context;
}

int Cy_SysInt_Init(const cy_stc_sysint_t *cfg, void (*isr)(void))
{
    (void)cfg;
    (void)isr;
    return 0;
}

void NVIC_EnableIRQ(int irqn)
{
    (void)irqn;
}

void NVIC_DisableIRQ(int irqn)
{
    (void)irqn;
}

uint32_t Cy_SysClk_ClkPeriGetFrequency(void)
{
    return 100000000u;
}

void Cy_SysClk_PeriphDisableDivider(uint32_t hw, uint32_t num)
{
    (void)hw;
    (void)num;
}

void Cy_SysClk_PeriphSetDivider(uint32_t hw, uint32_t num, uint32_t val)
{
    (void)hw;
    (void)num;
    (void)val;
}

void Cy_SysClk_PeriphEnableDivider(uint32_t hw, uint32_t num)
{
    (void)hw;
    (void)num;
}

uint32_t Cy_GPIO_Read(void *port, uint32_t pin)
{
    (void)pin;
    if (port == CYBSP_I2C_SCL_PORT)
    {
        return s_scl_level;
    }
    if (port == CYBSP_I2C_SDA_PORT)
    {
        return s_sda_level;
    }
    return 1u;
}

en_hsiom_sel_t Cy_GPIO_GetHSIOM(void *port, uint32_t pin)
{
    (void)port;
    (void)pin;
    return s_scl_hsiom;
}

void Cy_GPIO_SetHSIOM(void *port, uint32_t pin, en_hsiom_sel_t hsiom)
{
    (void)pin;
    if (port == CYBSP_I2C_SCL_PORT)
    {
        s_scl_hsiom = hsiom;
    }
}

void Cy_GPIO_SetDrivemode(void *port, uint32_t pin, uint32_t mode)
{
    (void)port;
    (void)pin;
    (void)mode;
}

void Cy_GPIO_Write(void *port, uint32_t pin, uint32_t value)
{
    (void)pin;
    if (port == CYBSP_I2C_SCL_PORT)
    {
        s_scl_level = value;
        s_write_calls++;
    }
}

void Cy_SysLib_DelayUs(uint32_t us)
{
    (void)us;
    s_delay_us_calls++;
}
