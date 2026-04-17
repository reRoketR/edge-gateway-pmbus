/**
 * @file main.c
 * @brief PMBus Target (slave) simulator for KIT_PSC3M5_EVK.
 *
 * @details
 * Simulates a 48 V-in / 12 V-out power supply module using the Infineon
 * `mtb-pmbus` middleware.  Responds to PMBus read commands from the gateway
 * (master) with slowly-varying sinusoidal telemetry values.
 *
 * ### Supported PMBus Commands
 * | Command              | Code   | Format     |
 * |----------------------|--------|------------|
 * | CLEAR_FAULTS         | 0x03   | Send Byte  |
 * | VOUT_MODE            | 0x20   | Read Byte  |
 * | READ_VIN             | 0x88   | Read Word  |
 * | READ_VOUT            | 0x8B   | Read Word  |
 * | READ_IIN             | 0x89   | Read Word  |
 * | READ_IOUT            | 0x8C   | Read Word  |
 * | READ_TEMPERATURE_1   | 0x8D   | Read Word  |
 * | READ_POUT            | 0x96   | Read Word  |
 * | STATUS_WORD          | 0x79   | Read Word  |
 * | STATUS_VOUT          | 0x7A   | Read Byte  |
 * | STATUS_IOUT          | 0x7B   | Read Byte  |
 * | STATUS_TEMPERATURE   | 0x7D   | Read Byte  |
 *
 * ### Hardware Configuration
 * - Target address : 0x58
 * - PEC            : ON
 * - I2C Slave      : SCB0, P9_0 (SCL) / P9_2 (SDA)
 * - Debug UART     : SCB3, P6_2 (RX) / P6_3 (TX)
 *
 * @author Volodymyr
 * @copyright (c) 2024–2025
 */

/*******************************************************************************
* Header Files
*******************************************************************************/
#include "cy_pdl.h"
#include "cybsp.h"
#include "cy_retarget_io.h"
#include "mtb_hal.h"
#include "mtb_pmbus.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

/*******************************************************************************
* Macros
*******************************************************************************/

/** @brief PMBus 7-bit slave address — must match Device Configurator. */
#define PMBUS_DEVICE_ADDRESS        (0x58U)

/** @name SMBALERT line — D2c-2
 *  Open-drain output to signal faults to the gateway.
 *  @{ */
#define SMBALERT_PORT   GPIO_PRT3
#define SMBALERT_PIN    0U
/** @} */

/** @name PMBus Command Codes
 *  Must match the gateway telemetry.h definitions.
 *  @{ */
#define CMD_VOUT_MODE               (0x20U) /**< Output voltage mode (Linear16 exponent). */
#define CMD_STATUS_WORD             (0x79U) /**< Summary status register (2 bytes). */
#define CMD_STATUS_VOUT             (0x7AU) /**< Output voltage fault/warning status (1 byte). */
#define CMD_STATUS_IOUT             (0x7BU) /**< Output current fault/warning status (1 byte). */
#define CMD_STATUS_TEMPERATURE      (0x7DU) /**< Temperature fault/warning status (1 byte). */
#define CMD_READ_VIN                (0x88U) /**< Input voltage — Linear11. */
#define CMD_READ_VOUT               (0x8BU) /**< Output voltage — Linear16. */
#define CMD_READ_IIN                (0x89U) /**< Input current — Linear11. */
#define CMD_READ_IOUT               (0x8CU) /**< Output current — Linear11. */
#define CMD_READ_TEMPERATURE_1      (0x8DU) /**< Temperature sensor 1 — Linear11. */
#define CMD_READ_POUT               (0x96U) /**< Output power — Linear11. */
#define CMD_CLEAR_FAULTS            (0x03U) /**< Clear all fault latches (Send Byte, no data). */
/** @} */

/** @brief Number of custom commands in cmd_table[]. */
#define PMBUS_CMD_TABLE_SIZE        (12U)

/** @brief VOUT_MODE exponent for Linear16 encoding.
 *  Real voltage = mantissa × 2^VOUT_EXPONENT.  With −12 the LSB is ~244 µV. */
#define VOUT_EXPONENT   (-12)

/*******************************************************************************
* Simulated Telemetry — slowly varying to look realistic
*******************************************************************************/
/** @brief Monotonic tick counter, incremented by the timer ISR (~500 ms period). */
static volatile uint32_t s_tick_counter = 0u;

/** @name Simulated Telemetry Functions
 *  Slowly-varying sinusoidal outputs that model a 48 V-in / 12 V-out PSU
 *  at approximately 100 W output power.
 *  @note VOUT uses ULINEAR16 (unsigned) — values are kept strictly positive.
 *  @{ */

/** @brief Simulated input voltage (nominal 48 V, ±0.5 V ripple). */
static float get_sim_vin(uint32_t t)  { return 48.0f + 0.5f  * sinf((float)t * 0.01f); }
/** @brief Simulated output voltage (nominal 12 V, ±0.15 V ripple). */
static float get_sim_vout(uint32_t t) { return 12.0f + 0.15f * sinf((float)t * 0.013f); }
/** @brief Simulated input current (nominal 2.1 A, ±0.1 A ripple). */
static float get_sim_iin(uint32_t t)  { return 2.1f  + 0.1f  * sinf((float)t * 0.017f); }
/** @brief Simulated output current (nominal 8.3 A, ±0.3 A ripple). */
static float get_sim_iout(uint32_t t) { return 8.3f  + 0.3f  * sinf((float)t * 0.019f); }
/** @brief Simulated temperature (nominal 42.5 °C, ±2.0 °C variation). */
static float get_sim_temp(uint32_t t) { return 42.5f + 2.0f  * sinf((float)t * 0.007f); }
/** @brief Simulated output power — computed as VOUT × IOUT. */
static float get_sim_pout(uint32_t t) { return get_sim_vout(t) * get_sim_iout(t); }
/** @} */

/** @name PMBus Command Data Buffers
 *  Initial data buffers used by mtb_pmbus_init(). After initialization,
 *  values are updated via mtb_pmbus_cmd_update_data() from the main loop.
 *  @{ */
static uint8_t buf_vout_mode[1]     = { 0 }; /**< VOUT_MODE register (1 byte). */
static uint8_t buf_read_vin[2]      = { 0 }; /**< READ_VIN Linear11 (2 bytes LE). */
static uint8_t buf_read_vout[2]     = { 0 }; /**< READ_VOUT Linear16 (2 bytes LE). */
static uint8_t buf_read_iin[2]      = { 0 }; /**< READ_IIN Linear11 (2 bytes LE). */
static uint8_t buf_read_iout[2]     = { 0 }; /**< READ_IOUT Linear11 (2 bytes LE). */
static uint8_t buf_read_temp1[2]    = { 0 }; /**< READ_TEMPERATURE_1 Linear11 (2 bytes LE). */
static uint8_t buf_read_pout[2]     = { 0 }; /**< READ_POUT Linear11 (2 bytes LE). */
static uint8_t buf_status_word[2]   = { 0 }; /**< STATUS_WORD bitmask (2 bytes LE). */
static uint8_t buf_status_vout[1]   = { 0 }; /**< STATUS_VOUT bitmask (1 byte). */
static uint8_t buf_status_iout[1]   = { 0 }; /**< STATUS_IOUT bitmask (1 byte). */
static uint8_t buf_status_temp[1]   = { 0 }; /**< STATUS_TEMPERATURE bitmask (1 byte). */
static uint8_t buf_clear_faults[1]  = { 0 }; /**< CLEAR_FAULTS dummy buffer (write-only, no-op). */
/** @} */

/**
 * @brief Store a 16-bit value into a 2-byte buffer in little-endian order.
 *
 * @param[out] buf  Pointer to at least 2 bytes of storage.
 * @param[in]  val  The 16-bit value to store.
 */
static inline void store_le16(uint8_t *buf, uint16_t val)
{
    buf[0] = (uint8_t)(val & 0xFFu);
    buf[1] = (uint8_t)(val >> 8);
}

/*******************************************************************************
* SMBALERT latched fault state (D2c-2)
*
* UART commands set/clear these flags only.  update_simulated_registers()
* derives STATUS_WORD from s_fault_latched — never writes directly.
*******************************************************************************/

/** CML fault latched — drives STATUS_WORD bit 1 until cleared. */
static volatile bool s_fault_latched  = false;
/** SMBALERT line is actively driven LOW by the middleware. */
static volatile bool s_alert_asserted = false;

/**
 * @brief Update all simulated PMBus registers using the middleware API.
 *
 * Uses mtb_pmbus_float_to_lin11() / mtb_pmbus_float_to_lin16() for encoding
 * and mtb_pmbus_cmd_update_data() for safe buffer updates (avoids data
 * corruption when a command is being read by the controller).
 *
 * Must NOT be called from ISR context — mtb_pmbus_cmd_update_data() will
 * have no effect if the command is currently active.
 */
static void update_simulated_registers(mtb_pmbus_stc_t *inst)
{
    uint8_t  tmp1[1];
    uint8_t  tmp2[2];
    uint16_t word;

    /* Snapshot the volatile tick counter once so all simulated values are
     * computed from the same instant — avoids slight inconsistencies when
     * the timer ISR fires between individual get_sim_*() calls. */
    const uint32_t tick = s_tick_counter;

    /* VOUT_MODE: mode=0 (linear), exponent in bits[4:0] */
    tmp1[0] = (uint8_t)(VOUT_EXPONENT & 0x1Fu);
    mtb_pmbus_cmd_update_data(inst, CMD_VOUT_MODE, tmp1, sizeof(tmp1));

    /* READ_VIN — Linear11 */
    word = mtb_pmbus_float_to_lin11(get_sim_vin(tick));
    store_le16(tmp2, word);
    mtb_pmbus_cmd_update_data(inst, CMD_READ_VIN, tmp2, sizeof(tmp2));

    /* READ_VOUT — Linear16 */
    word = mtb_pmbus_float_to_lin16(get_sim_vout(tick), (int8_t)VOUT_EXPONENT);
    store_le16(tmp2, word);
    mtb_pmbus_cmd_update_data(inst, CMD_READ_VOUT, tmp2, sizeof(tmp2));

    /* READ_IIN — Linear11 */
    word = mtb_pmbus_float_to_lin11(get_sim_iin(tick));
    store_le16(tmp2, word);
    mtb_pmbus_cmd_update_data(inst, CMD_READ_IIN, tmp2, sizeof(tmp2));

    /* READ_IOUT — Linear11 */
    word = mtb_pmbus_float_to_lin11(get_sim_iout(tick));
    store_le16(tmp2, word);
    mtb_pmbus_cmd_update_data(inst, CMD_READ_IOUT, tmp2, sizeof(tmp2));

    /* READ_TEMPERATURE_1 — Linear11 */
    word = mtb_pmbus_float_to_lin11(get_sim_temp(tick));
    store_le16(tmp2, word);
    mtb_pmbus_cmd_update_data(inst, CMD_READ_TEMPERATURE_1, tmp2, sizeof(tmp2));

    /* READ_POUT — Linear11 */
    word = mtb_pmbus_float_to_lin11(get_sim_pout(tick));
    store_le16(tmp2, word);
    mtb_pmbus_cmd_update_data(inst, CMD_READ_POUT, tmp2, sizeof(tmp2));

    /* Status registers — derived from latched fault state (D2c-2) */
    {
        uint16_t status_word = 0x0000u;
        if (s_fault_latched)
        {
            status_word = 0x0002u;  /* bit 1 = CML fault */
        }
        store_le16(tmp2, status_word);
        mtb_pmbus_cmd_update_data(inst, CMD_STATUS_WORD, tmp2, sizeof(tmp2));
    }

    tmp1[0] = 0x00u;
    mtb_pmbus_cmd_update_data(inst, CMD_STATUS_VOUT, tmp1, sizeof(tmp1));
    mtb_pmbus_cmd_update_data(inst, CMD_STATUS_IOUT, tmp1, sizeof(tmp1));
    mtb_pmbus_cmd_update_data(inst, CMD_STATUS_TEMPERATURE, tmp1, sizeof(tmp1));
}

/*******************************************************************************
* ISR-deferred flags for pmbus callbacks (printf is NOT ISR-safe)
*
* The PMBus middleware invokes callbacks from I2C ISR context.  Instead of
* calling printf (which uses the UART SCB and may block), we set lightweight
* volatile flags here and print the deferred messages from the main loop.
*******************************************************************************/

/** Bitmask of pending error events (ISR → main loop). */
static volatile uint32_t g_pending_error_events = 0u;
/** Command code associated with the most recent error (best-effort). */
static volatile uint8_t  g_error_cmd_code       = 0u;

/** Bitmask of pending general events. */
#define GEN_EVT_QUICK_WR  (1u << 0)
#define GEN_EVT_QUICK_RD  (1u << 1)
#define GEN_EVT_ARA_READ  (1u << 2)
static volatile uint32_t g_pending_gen_events    = 0u;

/*******************************************************************************
* PMBus Callbacks
*******************************************************************************/

/**
 * @brief Per-command callback invoked from ISR context on read/write events.
 *
 * @details
 * This callback is registered for every command in cmd_table[].  It is
 * intentionally a no-op because mtb_pmbus_cmd_update_data() has no effect
 * when called from ISR context.  Register updates are instead performed
 * periodically from the main loop via update_simulated_registers().
 *
 * @param[in] event  The PMBus command event (read started, write completed, etc.).
 * @param[in] page   PMBus page number (unused — single-page device).
 * @param[in] phase  PMBus phase number (unused).
 * @param[in] byte   Command-specific byte (unused).
 *
 * @return Always @c true to indicate the command is accepted.
 */
static bool cmd_telemetry_callback(mtb_pmbus_cmd_events_t event,
                                   int32_t page, int32_t phase,
                                   uint8_t byte)
{
    (void)page;
    (void)phase;
    (void)byte;
    (void)event;

    return true;
}

/**
 * @brief CLEAR_FAULTS callback — clears the latched CML fault and SMBALERT.
 *
 * Called from ISR context when the master sends CLEAR_FAULTS (0x03).
 * Only lightweight flag operations here; the SMBALERT line will be
 * released by the middleware in AUTO mode or on the next register update.
 */
static bool cmd_clear_faults_callback(mtb_pmbus_cmd_events_t event,
                                      int32_t page, int32_t phase,
                                      uint8_t byte)
{
    (void)page;
    (void)phase;
    (void)byte;

    if (event == MTB_PMBUS_CMD_WRITE_DONE)
    {
        s_fault_latched  = false;
        s_alert_asserted = false;
    }
    return true;
}

/**
 * @brief General PMBus event callback for Quick Commands (ISR context).
 *
 * @details
 * Sets a flag for the main loop to print.  Cy_GPIO_Inv is ISR-safe
 * (single register write) so LED toggle is done directly.
 *
 * @param[in] event  The general PMBus event type.
 */
static void pmbus_gen_callback(mtb_pmbus_events_t event)
{
    if (event == MTB_PMBUS_QUICK_CMD_WR_EVENT)
    {
        g_pending_gen_events |= GEN_EVT_QUICK_WR;
        Cy_GPIO_Inv(CYBSP_USER_LED_PORT, CYBSP_USER_LED_PIN);
    }
    else if (event == MTB_PMBUS_QUICK_CMD_RD_EVENT)
    {
        g_pending_gen_events |= GEN_EVT_QUICK_RD;
    }
#if MTB_PMBUS_SUPPORT_SMBALERT
    else if (event == MTB_PMBUS_ALERT_RESPONSE_ADDR_EVENT)
    {
        /* AUTO mode — middleware already cleared the SMBALERT line */
        s_alert_asserted = false;
        g_pending_gen_events |= GEN_EVT_ARA_READ;
    }
#endif
}

/**
 * @brief PMBus error callback — ISR-safe (flag-based, no printf).
 *
 * @details
 * Called by the middleware from I2C ISR context when the target detects
 * an error condition.  Stores the event bitmask and command code in
 * volatile variables for deferred printing in the main loop.
 *
 * @param[in] events      Bitmask of error flags (MTB_PMBUS_ERR_*).
 * @param[in] cmd_code    The PMBus command code that triggered the error.
 * @param[in] cmd_is_ext  Whether the command is extended (unused).
 */
static void pmbus_error_callback(uint32_t events, uint8_t cmd_code, bool cmd_is_ext)
{
    (void)cmd_is_ext;

    g_pending_error_events |= events;
    g_error_cmd_code        = cmd_code;
}

/*******************************************************************************
* PMBus Command Table — matches gateway polling commands
*******************************************************************************/
static mtb_pmbus_stc_config_cmd_t cmd_table[PMBUS_CMD_TABLE_SIZE] =
{
    /* VOUT_MODE — Read Byte, 1 byte */
    {
        .cmd_code  = CMD_VOUT_MODE,
        .cmd_cap   = MTB_PMBUS_CMD_CAP_DIR_RD | MTB_PMBUS_CMD_CAP_FORMAT_8_BIT,
        .data_buf  = buf_vout_mode,
        .data_size = sizeof(buf_vout_mode),
        .callback  = cmd_telemetry_callback,
    },
    /* READ_VIN — Read Word, Linear11 */
    {
        .cmd_code  = CMD_READ_VIN,
        .cmd_cap   = MTB_PMBUS_CMD_CAP_DIR_RD | MTB_PMBUS_CMD_CAP_FORMAT_LIN_11_16,
        .data_buf  = buf_read_vin,
        .data_size = sizeof(buf_read_vin),
        .callback  = cmd_telemetry_callback,
    },
    /* READ_VOUT — Read Word, Linear16 */
    {
        .cmd_code  = CMD_READ_VOUT,
        .cmd_cap   = MTB_PMBUS_CMD_CAP_DIR_RD | MTB_PMBUS_CMD_CAP_FORMAT_LIN_11_16,
        .data_buf  = buf_read_vout,
        .data_size = sizeof(buf_read_vout),
        .callback  = cmd_telemetry_callback,
    },
    /* READ_IIN — Read Word, Linear11 */
    {
        .cmd_code  = CMD_READ_IIN,
        .cmd_cap   = MTB_PMBUS_CMD_CAP_DIR_RD | MTB_PMBUS_CMD_CAP_FORMAT_LIN_11_16,
        .data_buf  = buf_read_iin,
        .data_size = sizeof(buf_read_iin),
        .callback  = cmd_telemetry_callback,
    },
    /* READ_IOUT — Read Word, Linear11 */
    {
        .cmd_code  = CMD_READ_IOUT,
        .cmd_cap   = MTB_PMBUS_CMD_CAP_DIR_RD | MTB_PMBUS_CMD_CAP_FORMAT_LIN_11_16,
        .data_buf  = buf_read_iout,
        .data_size = sizeof(buf_read_iout),
        .callback  = cmd_telemetry_callback,
    },
    /* READ_TEMPERATURE_1 — Read Word, Linear11 */
    {
        .cmd_code  = CMD_READ_TEMPERATURE_1,
        .cmd_cap   = MTB_PMBUS_CMD_CAP_DIR_RD | MTB_PMBUS_CMD_CAP_FORMAT_LIN_11_16,
        .data_buf  = buf_read_temp1,
        .data_size = sizeof(buf_read_temp1),
        .callback  = cmd_telemetry_callback,
    },
    /* READ_POUT — Read Word, Linear11 */
    {
        .cmd_code  = CMD_READ_POUT,
        .cmd_cap   = MTB_PMBUS_CMD_CAP_DIR_RD | MTB_PMBUS_CMD_CAP_FORMAT_LIN_11_16,
        .data_buf  = buf_read_pout,
        .data_size = sizeof(buf_read_pout),
        .callback  = cmd_telemetry_callback,
    },
    /* STATUS_WORD — Read Word, non-numeric */
    {
        .cmd_code  = CMD_STATUS_WORD,
        .cmd_cap   = MTB_PMBUS_CMD_CAP_DIR_RD | MTB_PMBUS_CMD_CAP_FORMAT_NO_NUM,
        .data_buf  = buf_status_word,
        .data_size = sizeof(buf_status_word),
        .callback  = cmd_telemetry_callback,
    },
    /* STATUS_VOUT — Read Byte */
    {
        .cmd_code  = CMD_STATUS_VOUT,
        .cmd_cap   = MTB_PMBUS_CMD_CAP_DIR_RD | MTB_PMBUS_CMD_CAP_FORMAT_8_BIT,
        .data_buf  = buf_status_vout,
        .data_size = sizeof(buf_status_vout),
        .callback  = cmd_telemetry_callback,
    },
    /* STATUS_IOUT — Read Byte */
    {
        .cmd_code  = CMD_STATUS_IOUT,
        .cmd_cap   = MTB_PMBUS_CMD_CAP_DIR_RD | MTB_PMBUS_CMD_CAP_FORMAT_8_BIT,
        .data_buf  = buf_status_iout,
        .data_size = sizeof(buf_status_iout),
        .callback  = cmd_telemetry_callback,
    },
    /* STATUS_TEMPERATURE — Read Byte */
    {
        .cmd_code  = CMD_STATUS_TEMPERATURE,
        .cmd_cap   = MTB_PMBUS_CMD_CAP_DIR_RD | MTB_PMBUS_CMD_CAP_FORMAT_8_BIT,
        .data_buf  = buf_status_temp,
        .data_size = sizeof(buf_status_temp),
        .callback  = cmd_telemetry_callback,
    },
    /* CLEAR_FAULTS — Send Byte (write-only, no data) */
    {
        .cmd_code  = CMD_CLEAR_FAULTS,
        .cmd_cap   = MTB_PMBUS_CMD_CAP_DIR_WR | MTB_PMBUS_CMD_CAP_FORMAT_NO_NUM,
        .data_buf  = buf_clear_faults,
        .data_size = sizeof(buf_clear_faults),
        .callback  = cmd_clear_faults_callback,
    },
};

/*******************************************************************************
* I2C + PMBus Infrastructure
*******************************************************************************/

/** @brief I2C PDL context required by the mtb-pmbus HAL layer. */
static cy_stc_scb_i2c_context_t i2c_pdl_context;

/** @brief PMBus middleware instance — holds all runtime state. */
static mtb_pmbus_stc_t pmbus_inst;

/** @brief Size of the I2C slave read buffer for PMBus middleware internal use. */
#define PMBUS_READ_BUF_SIZE     (4U)
/** @brief I2C slave read buffer — consumed by PMBus middleware during transactions. */
static uint8_t pmbus_read_buf[PMBUS_READ_BUF_SIZE];

/** @brief PMBus HAL configuration — maps to SCB0 hardware. */
static mtb_pmbus_stc_config_hal_t pmbus_hal_cfg =
{
    .hw_ptr            = PMBUS_TARGET_HW,
    .pdl_i2c_context   = &i2c_pdl_context,
    .hal_read_buf_ptr  = pmbus_read_buf,
    .hal_read_buf_size = PMBUS_READ_BUF_SIZE,
#if MTB_PMBUS_SUPPORT_SMBALERT
    .smbalert_port_addr = SMBALERT_PORT,
    .smbalert_pin_num   = SMBALERT_PIN,
#endif
};

/**
 * @brief I2C interrupt handler — delegates to PMBus middleware.
 *
 * Registered as the NVIC ISR for SCB0.  All I2C slave event processing
 * is handled by mtb_pmbus_i2c_isr().
 */
static void i2c_isr(void)
{
    mtb_pmbus_i2c_isr(&pmbus_inst);
}

/**
 * @brief Hardware resource enable/disable callback for PMBus middleware.
 *
 * @details
 * Called by mtb_pmbus_enable() and mtb_pmbus_disable() to start or stop
 * the I2C SCB hardware.
 *
 * @param[in] action  MTB_PMBUS_HW_RESOURCES_ENABLE or _DISABLE.
 */
static void hw_resource_ctrl_callback(mtb_pmbus_hw_resources_ctrl_action_t action)
{
    if (action == MTB_PMBUS_HW_RESOURCES_ENABLE)
    {
        Cy_SCB_I2C_Enable(PMBUS_TARGET_HW);
    }
    else
    {
        Cy_SCB_I2C_Disable(PMBUS_TARGET_HW, &i2c_pdl_context);
    }
}

/** @brief Enable the I2C slave NVIC interrupt. */
static void hw_isr_enable(void)  { NVIC_EnableIRQ(PMBUS_TARGET_IRQ);  }
/** @brief Disable the I2C slave NVIC interrupt. */
static void hw_isr_disable(void) { NVIC_DisableIRQ(PMBUS_TARGET_IRQ); }

/** @brief PMBus hardware configuration — ISR and resource control callbacks. */
static mtb_pmbus_stc_config_hw_t pmbus_hw_cfg =
{
    .hal_config                = &pmbus_hal_cfg,
    .hw_resource_ctrl_callback = hw_resource_ctrl_callback,
    .enable_hw_irq_callback    = hw_isr_enable,
    .disable_hw_irq_callback   = hw_isr_disable,
};

/** @brief PMBus main configuration — address, PEC, command table, callbacks. */
static mtb_pmbus_stc_config_t pmbus_cfg =
{
    .hw_config          = &pmbus_hw_cfg,
    .address            = PMBUS_DEVICE_ADDRESS,
    .enable_pec         = true,
    .enable_pmbus       = true,
#if MTB_PMBUS_SUPPORT_SMBALERT
    .enable_smbalert    = true,
#endif
    .impl_cmd_mask      = MTB_PMBUS_IMPL_CMD_CAPABILITY_EN | MTB_PMBUS_IMPL_CMD_REVISION_EN,
    .cmd_table          = cmd_table,
    .cmd_num            = PMBUS_CMD_TABLE_SIZE,
    .gen_callback       = pmbus_gen_callback,
    .errors_callback    = pmbus_error_callback,
    .revision           = MTB_PMBUS_REVISION_1_4,
    .speed              = MTB_PMBUS_SPEED_100,
    .enable_ieee_format = false,
};

/*******************************************************************************
* Debug UART
*******************************************************************************/
static cy_stc_scb_uart_context_t DEBUG_UART_context;
static mtb_hal_uart_t            DEBUG_UART_hal_obj;

/*******************************************************************************
* Timer for LED blink + simulation tick
*******************************************************************************/
const cy_stc_sysint_t intrCfg1 =
{
    .intrSrc      = TCPWM_COUNTER_IRQ,
    .intrPriority = 7u
};

/** @brief Flag set by timer ISR, cleared in main loop (LED blink trigger). */
volatile bool timer_interrupt_flag = false;

/**
 * @brief Timer compare/capture ISR — increments the simulation tick counter.
 *
 * @details
 * Fires every ~500 ms (configured in Device Configurator).  Increments
 * s_tick_counter which drives all simulation waveforms, and sets
 * timer_interrupt_flag for LED toggling in the main loop.
 */
void isr_timer(void)
{
    uint32_t interrupts = Cy_TCPWM_GetInterruptStatusMasked(TCPWM_COUNTER_HW,
                                                            TCPWM_COUNTER_NUM);
    Cy_TCPWM_ClearInterrupt(TCPWM_COUNTER_HW, TCPWM_COUNTER_NUM, interrupts);

    if (0UL != (CY_TCPWM_INT_ON_CC0 & interrupts))
    {
        timer_interrupt_flag = true;
        s_tick_counter++;
    }
}

/**
 * @brief Initialize the TCPWM counter used as the simulation tick timer.
 *
 * @details
 * Configures the TCPWM counter for periodic interrupts (~500 ms).
 * Enables the NVIC interrupt and starts the counter.
 */
void timer_init(void)
{
    if (CY_TCPWM_SUCCESS != Cy_TCPWM_Counter_Init(TCPWM_COUNTER_HW,
                                                    TCPWM_COUNTER_NUM,
                                                    &TCPWM_COUNTER_config))
    {
        CY_ASSERT(0);
    }
    Cy_TCPWM_Counter_Enable(TCPWM_COUNTER_HW, TCPWM_COUNTER_NUM);
    Cy_TCPWM_SetInterruptMask(TCPWM_COUNTER_HW, TCPWM_COUNTER_NUM,
                               CY_TCPWM_INT_ON_CC0);
    Cy_SysInt_Init(&intrCfg1, isr_timer);
    NVIC_EnableIRQ(TCPWM_COUNTER_IRQ);
    Cy_TCPWM_TriggerStart_Single(TCPWM_COUNTER_HW, TCPWM_COUNTER_NUM);
}

/**
 * @brief Application entry point.
 *
 * @details
 * Initialization sequence:
 * 1. BSP and debug UART initialization
 * 2. I2C SCB0 slave initialization (without enabling — PMBus middleware
 *    handles enable via hw_resource_ctrl_callback)
 * 3. Prime simulated register buffers with initial values
 * 4. Initialize and enable PMBus middleware
 * 5. Start the simulation tick timer
 * 6. Enter the super-loop:
 *    - Toggle LED on timer tick
 *    - Update simulated PMBus registers every tick (~500 ms)
 *    - Print telemetry summary to UART every ~5 s
 *
 * @return Never returns — runs an infinite super-loop.
 */
int main(void)
{
    cy_rslt_t result;

#if defined(CY_DEVICE_SECURE)
    cyhal_wdt_t wdt_obj;
    result = cyhal_wdt_init(&wdt_obj, cyhal_wdt_get_max_timeout_ms());
    CY_ASSERT(CY_RSLT_SUCCESS == result);
    cyhal_wdt_free(&wdt_obj);
#endif

    /* ---- BSP init ---- */
    result = cybsp_init();
    CY_ASSERT(CY_RSLT_SUCCESS == result);

    __enable_irq();

    /* ---- Debug UART ---- */
    result = (cy_rslt_t)Cy_SCB_UART_Init(DEBUG_UART_HW, &DEBUG_UART_config,
                                          &DEBUG_UART_context);
    CY_ASSERT(CY_RSLT_SUCCESS == result);
    Cy_SCB_UART_Enable(DEBUG_UART_HW);

    result = mtb_hal_uart_setup(&DEBUG_UART_hal_obj, &DEBUG_UART_hal_config,
                                 &DEBUG_UART_context, NULL);
    CY_ASSERT(CY_RSLT_SUCCESS == result);

    result = cy_retarget_io_init(&DEBUG_UART_hal_obj);
    CY_ASSERT(CY_RSLT_SUCCESS == result);

    printf("\x1b[2J\x1b[;H");
    printf("============================================================\r\n");
    printf("  PMBus Target Simulator  |  KIT_PSC3M5_EVK\r\n");
    printf("============================================================\r\n");
    printf("  Build: %s %s\r\n", __DATE__, __TIME__);
    printf("  Address: 0x%02X  PEC: ON\r\n", PMBUS_DEVICE_ADDRESS);
    printf("  SCB0 I2C Slave  P9_0 (SCL) / P9_2 (SDA)\r\n");
#if MTB_PMBUS_SUPPORT_SMBALERT
    printf("  SMBALERT# : D7 (open-drain, AUTO mode)\r\n");
    printf("  Triggers  : UART 'a'=assert, 'c'=clear\r\n");
#endif
    printf("============================================================\r\n\r\n");

    /* ---- I2C init (do NOT enable — PMBus middleware does it) ---- */
    Cy_SCB_I2C_Init(PMBUS_TARGET_HW, &PMBUS_TARGET_config, &i2c_pdl_context);

    /* Configure I2C interrupt */
    cy_stc_sysint_t i2c_isr_cfg =
    {
        .intrSrc      = PMBUS_TARGET_IRQ,
        .intrPriority = 3U
    };
    Cy_SysInt_Init(&i2c_isr_cfg, i2c_isr);

    printf("[TARGET] I2C transport initialized (SCB0 Slave)\r\n");

#if MTB_PMBUS_SUPPORT_SMBALERT
    /* ---- SMBALERT GPIO — open-drain, idle HIGH (D2c-2) ---- */
    {
        cy_stc_gpio_pin_config_t smbalert_pin_cfg = {
            .outVal    = 1u,
            .driveMode = CY_GPIO_DM_OD_DRIVESLOW,
            .hsiom     = HSIOM_SEL_GPIO,
        };
        Cy_GPIO_Pin_Init(SMBALERT_PORT, SMBALERT_PIN, &smbalert_pin_cfg);
        printf("[TARGET] SMBALERT GPIO initialized (D7, open-drain)\r\n");
    }
#endif

    /* ---- Prime simulated register buffers (before middleware init,
     *      direct buffer access is safe) ---- */
    {
        uint16_t w;
        const uint32_t t0 = s_tick_counter;
        buf_vout_mode[0] = (uint8_t)(VOUT_EXPONENT & 0x1Fu);
        w = mtb_pmbus_float_to_lin11(get_sim_vin(t0));   store_le16(buf_read_vin,   w);
        w = mtb_pmbus_float_to_lin16(get_sim_vout(t0), (int8_t)VOUT_EXPONENT); store_le16(buf_read_vout, w);
        w = mtb_pmbus_float_to_lin11(get_sim_iin(t0));   store_le16(buf_read_iin,   w);
        w = mtb_pmbus_float_to_lin11(get_sim_iout(t0));  store_le16(buf_read_iout,  w);
        w = mtb_pmbus_float_to_lin11(get_sim_temp(t0));  store_le16(buf_read_temp1, w);
        w = mtb_pmbus_float_to_lin11(get_sim_pout(t0));  store_le16(buf_read_pout,  w);
        store_le16(buf_status_word, 0x0000u);
        buf_status_vout[0] = 0x00u;
        buf_status_iout[0] = 0x00u;
        buf_status_temp[0] = 0x00u;
    }

    /* ---- Initialize PMBus middleware ---- */
    mtb_pmbus_status_t pmbus_status = mtb_pmbus_init(&pmbus_inst, &pmbus_cfg);
    if (MTB_PMBUS_STATUS_SUCCESS != pmbus_status)
    {
        printf("[TARGET] ERROR: mtb_pmbus_init failed (0x%lX)\r\n",
               (unsigned long)pmbus_status);
        CY_ASSERT(0);
    }

    pmbus_status = mtb_pmbus_enable(&pmbus_inst);
    if (MTB_PMBUS_STATUS_SUCCESS != pmbus_status)
    {
        printf("[TARGET] ERROR: mtb_pmbus_enable failed (0x%lX)\r\n",
               (unsigned long)pmbus_status);
        CY_ASSERT(0);
    }

    printf("[TARGET] PMBus middleware initialized and enabled\r\n");
    printf("[TARGET] Commands registered: %u\r\n", PMBUS_CMD_TABLE_SIZE);
#if MTB_PMBUS_SUPPORT_SMBALERT
    mtb_pmbus_smbalert_config_mode(&pmbus_inst, MTB_PMBUS_SMBALERT_MODE_AUTO);
    printf("[TARGET] SMBALERT mode: AUTO (cleared after ARA)\r\n");
#endif
    printf("[TARGET] Waiting for controller reads...\r\n\r\n");

    /* ---- Timer for LED blink ---- */
    timer_init();

    /* ---- Main loop ---- */
    uint32_t last_update_tick = 0u;
    uint32_t last_print_tick  = 0u;

    for (;;)
    {
        /* Blink LED */
        if (timer_interrupt_flag)
        {
            timer_interrupt_flag = false;
            Cy_GPIO_Inv(CYBSP_USER_LED_PORT, CYBSP_USER_LED_PIN);
        }

        /* ---- Deferred prints from ISR callbacks ---- */
        {
            uint32_t errs = g_pending_error_events;
            if (errs != 0u)
            {
                uint8_t cmd = g_error_cmd_code;
                g_pending_error_events = 0u;

                if (errs & MTB_PMBUS_ERR_UNSUPPORTED_CMD)
                    printf("[TARGET] ERR: unsupported cmd 0x%02X\r\n", cmd);
                if (errs & MTB_PMBUS_ERR_CORRUPTED_DATA)
                    printf("[TARGET] ERR: PEC mismatch cmd 0x%02X\r\n", cmd);
                if (errs & MTB_PMBUS_ERR_BUS_ERROR)
                    printf("[TARGET] ERR: bus error cmd 0x%02X\r\n", cmd);
            }

            uint32_t gen = g_pending_gen_events;
            if (gen != 0u)
            {
                g_pending_gen_events = 0u;
                if (gen & GEN_EVT_QUICK_WR)
                    printf("[TARGET] Quick Command (Write)\r\n");
                if (gen & GEN_EVT_QUICK_RD)
                    printf("[TARGET] Quick Command (Read)\r\n");
                if (gen & GEN_EVT_ARA_READ)
                    printf("[TARGET] ARA read — alert auto-cleared\r\n");
            }
        }

#if MTB_PMBUS_SUPPORT_SMBALERT
        /* ---- UART SMBALERT trigger (D2c-2) ---- */
        if (Cy_SCB_UART_GetNumInRxFifo(DEBUG_UART_HW) > 0u)
        {
            uint32_t ch = Cy_SCB_UART_Get(DEBUG_UART_HW);
            if (ch == (uint32_t)'a' || ch == (uint32_t)'A')
            {
                s_fault_latched  = true;
                s_alert_asserted = true;
                mtb_pmbus_smbalert_set_signal(&pmbus_inst);
                printf("[TARGET] SMBALERT asserted (CML fault simulated)\r\n");
            }
            else if (ch == (uint32_t)'c' || ch == (uint32_t)'C')
            {
                s_fault_latched  = false;
                s_alert_asserted = false;
                mtb_pmbus_smbalert_clear_signal(&pmbus_inst);
                printf("[TARGET] SMBALERT cleared\r\n");
            }
        }
#endif

        uint32_t now = s_tick_counter;

        /* Update PMBus data buffers every tick (~500 ms) so the
         * I2C analyzer / gateway always reads fresh telemetry.      */
        if (now != last_update_tick)
        {
            last_update_tick = now;
            update_simulated_registers(&pmbus_inst);
        }

        /* Print to UART every ~5 seconds (cosmetic only) */
        if ((now - last_print_tick) >= 10u)   /* 10 × ~500 ms = 5 s */
        {
            last_print_tick = now;
            const uint32_t tp = s_tick_counter;
            printf("[SIM] VIN=%.1f V  VOUT=%.2f V  IIN=%.2f A  "
                   "IOUT=%.1f A  TEMP=%.1f C  POUT=%.1f W\r\n",
                   (double)get_sim_vin(tp),
                   (double)get_sim_vout(tp),
                   (double)get_sim_iin(tp),
                   (double)get_sim_iout(tp),
                   (double)get_sim_temp(tp),
                   (double)get_sim_pout(tp));
        }
    }
}

/* [] END OF FILE */
