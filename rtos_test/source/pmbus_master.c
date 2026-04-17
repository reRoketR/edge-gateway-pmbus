/*******************************************************************************
 * File Name:   pmbus_master.c
 *
 * Description: PMBus/SMBus master driver implementation using PDL SCB I2C.
 *              Operates on SCB3 (PMBUS_CONTROLLER) configured by Device
 *              Configurator as I2C master.
 *
 *              Initialization sequence (PDL):
 *                1. Cy_SCB_I2C_Init(SCB3, &PMBUS_CONTROLLER_config, &ctx)
 *                2. Cy_SCB_I2C_SetDataRate(SCB3, speed_hz, scb_clk_hz)
 *                3. Enable SCB3 interrupt (for high-level MasterWrite/Read)
 *                4. Cy_SCB_I2C_Enable(SCB3)
 *
 *              SMBus "Read Word" protocol uses the high-level API:
 *                - MasterWrite (cmd byte, xferPending=true)  → no STOP
 *                - MasterRead  (2 bytes,  xferPending=false) → STOP
 *                Both are interrupt-driven and blocking with timeout.
 *
 * Related Document: agent.md §3
 *
 ******************************************************************************/

#include "pmbus_master.h"
#include "gateway_config.h"
#include "gateway_ipc.h"
#include "metrics.h"

/* PDL I2C driver */
#include "cy_scb_i2c.h"
#include "cy_sysint.h"
#include "cy_sysclk.h"

/* Device Configurator generated: PMBUS_CONTROLLER on SCB3 */
#include "cycfg_peripherals.h"
#include "cycfg_clocks.h"      /* PMBUS_CONTROLLER_CLK_HW / _NUM */

/* For GPIO bus-recovery */
#include "cy_gpio.h"
#include "cycfg_pins.h"
#include "cy_syslib.h"

/* FreeRTOS — needed for vTaskDelay (RTOS-friendly wait) */
#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>
#include <string.h>

/*******************************************************************************
 * Private data
 ******************************************************************************/

/** PDL I2C context — required by all Cy_SCB_I2C_Master* functions */
static cy_stc_scb_i2c_context_t pmbus_i2c_ctx;

/** Initialization flag */
static bool pmbus_initialized = false;

/** Timeout for PDL high-level transfer, set from g_config at init */
static uint32_t pmbus_transaction_timeout_ms;

/** Give abort a short chance to complete before resetting the SCB block. */
#define PMBUS_ABORT_SETTLE_MS (2u)

/** Shared-bus backoff after a skipped controller reset on non-idle lines. */
#define PMBUS_BUS_BACKOFF_MS  (500u)

/** Global bus backoff deadline shared across all PMBus devices. */
static TickType_t pmbus_bus_backoff_until = 0;

/*******************************************************************************
 * ISR for SCB3 (PMBUS_CONTROLLER)
 ******************************************************************************/
static void pmbus_scb3_isr(void)
{
    Cy_SCB_I2C_Interrupt(PMBUS_CONTROLLER_HW, &pmbus_i2c_ctx);
}

static const cy_stc_sysint_t pmbus_scb3_irq_cfg = {
    .intrSrc = PMBUS_CONTROLLER_IRQ,
    .intrPriority = 5u,  /* Mid-priority; below FreeRTOS ceiling (configMAX_SYSCALL_INTERRUPT_PRIORITY) */
};

/*******************************************************************************
 * Private helpers
 ******************************************************************************/
static void read_i2c_pin_snapshot(uint32_t *scl_level, uint32_t *sda_level,
                                  uint32_t *scl_hsiom, uint32_t *sda_hsiom);
static void log_i2c_bus_snapshot(const char *tag, uint32_t scb_status);
static void arm_bus_backoff(const char *reason);
static void wait_for_bus_backoff(void);
static bool wait_for_master_not_busy(uint32_t timeout_ms);
static bool pmbus_reset_controller_if_idle(const char *reason);
static void apply_recovery_settle_delay(void);

/**
 * @brief Wait for a PDL I2C master transfer to complete.
 *
 * Polls Cy_SCB_I2C_MasterGetStatus() until:
 *   - transfer completes (no CY_SCB_I2C_MASTER_BUSY), or
 *   - timeout_ms elapses.
 *
 * @return PMBUS_OK, PMBUS_ERR_NACK, PMBUS_ERR_TIMEOUT, PMBUS_ERR_ARB_LOST,
 *         or PMBUS_ERR_BUS_FAULT.
 */
static pmbus_status_t wait_for_completion(uint32_t timeout_ms)
{
    uint32_t elapsed = 0u;

    while (elapsed < timeout_ms)
    {
        uint32_t status = Cy_SCB_I2C_MasterGetStatus(PMBUS_CONTROLLER_HW,
                                                     &pmbus_i2c_ctx);

        if (0u == (status & CY_SCB_I2C_MASTER_BUSY))
        {
            /* Transfer done — check error flags */
            if (status & (CY_SCB_I2C_MASTER_ADDR_NAK | CY_SCB_I2C_MASTER_DATA_NAK))
            {
                return PMBUS_ERR_NACK;
            }
            if (status & CY_SCB_I2C_MASTER_ARB_LOST)
            {
                return PMBUS_ERR_ARB_LOST;
            }
            if (status & CY_SCB_I2C_MASTER_BUS_ERR)
            {
                return PMBUS_ERR_BUS_FAULT;
            }
            return PMBUS_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
        elapsed++;
    }

    /* Timeout — abort the pending transfer */
    log_i2c_bus_snapshot("timeout-before-abort",
                         Cy_SCB_I2C_MasterGetStatus(PMBUS_CONTROLLER_HW,
                                                    &pmbus_i2c_ctx));
    Cy_SCB_I2C_MasterAbortWrite(PMBUS_CONTROLLER_HW, &pmbus_i2c_ctx);
    Cy_SCB_I2C_MasterAbortRead(PMBUS_CONTROLLER_HW, &pmbus_i2c_ctx);
    log_i2c_bus_snapshot("timeout-after-abort",
                         Cy_SCB_I2C_MasterGetStatus(PMBUS_CONTROLLER_HW,
                                                    &pmbus_i2c_ctx));
    if (!wait_for_master_not_busy(PMBUS_ABORT_SETTLE_MS))
    {
        log_i2c_bus_snapshot("timeout-still-busy",
                             Cy_SCB_I2C_MasterGetStatus(PMBUS_CONTROLLER_HW,
                                                        &pmbus_i2c_ctx));
        (void)pmbus_reset_controller_if_idle("timeout");
    }
    return PMBUS_ERR_TIMEOUT;
}

/**
 * @brief Map PDL return value to pmbus_status_t.
 */
static pmbus_status_t map_pdl_status(cy_en_scb_i2c_status_t pdl_st)
{
    switch (pdl_st)
    {
        case CY_SCB_I2C_SUCCESS:          return PMBUS_OK;
        case CY_SCB_I2C_MASTER_NOT_READY: return PMBUS_ERR_NOT_READY;
        case CY_SCB_I2C_BAD_PARAM:        return PMBUS_ERR_ARG;
        default:                          return PMBUS_ERR_BUS_FAULT;
    }
}

static const char *pmbus_status_str(pmbus_status_t status)
{
    switch (status)
    {
        case PMBUS_OK:                return "OK";
        case PMBUS_ERR_NACK:          return "NACK";
        case PMBUS_ERR_TIMEOUT:       return "TIMEOUT";
        case PMBUS_ERR_ARB_LOST:      return "ARB_LOST";
        case PMBUS_ERR_BUS_FAULT:     return "BUS_FAULT";
        case PMBUS_ERR_NOT_READY:     return "NOT_READY";
        case PMBUS_ERR_RECOVERY_FAIL: return "RECOVERY_FAIL";
        case PMBUS_ERR_PEC:           return "PEC";
        case PMBUS_ERR_ARG:           return "ARG";
        case PMBUS_ERR_NOT_INIT:      return "NOT_INIT";
        case PMBUS_ERR_INIT:          return "INIT";
        default:                      return "UNKNOWN";
    }
}

static bool should_attempt_bus_recovery(pmbus_status_t status)
{
    return status == PMBUS_ERR_BUS_FAULT;
}

static bool should_attempt_controller_reset(pmbus_status_t status)
{
    return (status == PMBUS_ERR_TIMEOUT) || (status == PMBUS_ERR_NOT_READY);
}

static void arm_bus_backoff(const char *reason)
{
    TickType_t backoff_ticks = pdMS_TO_TICKS(PMBUS_BUS_BACKOFF_MS);
    TickType_t now = xTaskGetTickCount();
    TickType_t new_deadline;

    if (backoff_ticks == 0u)
    {
        backoff_ticks = 1u;
    }

    new_deadline = now + backoff_ticks;
    if ((int32_t)(new_deadline - pmbus_bus_backoff_until) > 0)
    {
        pmbus_bus_backoff_until = new_deadline;
        printf("[PMBUS] WARN: shared-bus backoff %lu ms after %s\n",
               (unsigned long)PMBUS_BUS_BACKOFF_MS,
               reason);
    }
}

static void read_i2c_pin_snapshot(uint32_t *scl_level, uint32_t *sda_level,
                                  uint32_t *scl_hsiom, uint32_t *sda_hsiom)
{
    if (scl_level != NULL)
    {
        *scl_level = Cy_GPIO_Read(CYBSP_I2C_SCL_PORT, CYBSP_I2C_SCL_PIN);
    }
    if (sda_level != NULL)
    {
        *sda_level = Cy_GPIO_Read(CYBSP_I2C_SDA_PORT, CYBSP_I2C_SDA_PIN);
    }
    if (scl_hsiom != NULL)
    {
        *scl_hsiom = (uint32_t)Cy_GPIO_GetHSIOM(CYBSP_I2C_SCL_PORT,
                                                CYBSP_I2C_SCL_PIN);
    }
    if (sda_hsiom != NULL)
    {
        *sda_hsiom = (uint32_t)Cy_GPIO_GetHSIOM(CYBSP_I2C_SDA_PORT,
                                                CYBSP_I2C_SDA_PIN);
    }
}

static void log_i2c_bus_snapshot(const char *tag, uint32_t scb_status)
{
    uint32_t scl_level;
    uint32_t sda_level;
    uint32_t scl_hsiom;
    uint32_t sda_hsiom;

    read_i2c_pin_snapshot(&scl_level, &sda_level, &scl_hsiom, &sda_hsiom);

    printf("[PMBUS] BUS: %s scb=0x%08lX scl=%lu sda=%lu hsiom_scl=%lu "
           "hsiom_sda=%lu\n",
           tag,
           (unsigned long)scb_status,
           (unsigned long)scl_level,
           (unsigned long)sda_level,
           (unsigned long)scl_hsiom,
           (unsigned long)sda_hsiom);
}

bool pmbus_bus_backoff_active(uint32_t *out_remaining_ms)
{
    TickType_t now;
    TickType_t remaining_ticks;
    uint32_t scl_level = 1u;
    uint32_t sda_level = 1u;

    now = xTaskGetTickCount();
    if ((int32_t)(pmbus_bus_backoff_until - now) <= 0)
    {
        pmbus_bus_backoff_until = 0u;
        if (out_remaining_ms != NULL)
        {
            *out_remaining_ms = 0u;
        }
        return false;
    }

    read_i2c_pin_snapshot(&scl_level, &sda_level, NULL, NULL);
    if ((scl_level != 0u) && (sda_level != 0u))
    {
        pmbus_bus_backoff_until = 0u;
        if (out_remaining_ms != NULL)
        {
            *out_remaining_ms = 0u;
        }
        return false;
    }

    remaining_ticks = pmbus_bus_backoff_until - now;
    if (out_remaining_ms != NULL)
    {
        *out_remaining_ms = (uint32_t)remaining_ticks * portTICK_PERIOD_MS;
        if ((*out_remaining_ms == 0u) && (remaining_ticks != 0u))
        {
            *out_remaining_ms = 1u;
        }
    }

    return true;
}

static void wait_for_bus_backoff(void)
{
    uint32_t remaining_ms = 0u;

    while (pmbus_bus_backoff_active(&remaining_ms))
    {
        vTaskDelay(pdMS_TO_TICKS(1u));
    }
}

static void apply_recovery_settle_delay(void)
{
    if (g_config.i2c.recovery_settle_ms != 0u)
    {
        vTaskDelay(pdMS_TO_TICKS(g_config.i2c.recovery_settle_ms));
    }
}

static bool wait_for_master_not_busy(uint32_t timeout_ms)
{
    uint32_t elapsed = 0u;

    while (elapsed < timeout_ms)
    {
        uint32_t scb_status = Cy_SCB_I2C_MasterGetStatus(PMBUS_CONTROLLER_HW,
                                                         &pmbus_i2c_ctx);
        if (0u == (scb_status & CY_SCB_I2C_MASTER_BUSY))
        {
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
        elapsed++;
    }

    return (0u == (Cy_SCB_I2C_MasterGetStatus(PMBUS_CONTROLLER_HW,
                                              &pmbus_i2c_ctx) &
                    CY_SCB_I2C_MASTER_BUSY));
}

static bool pmbus_reset_controller_if_idle(const char *reason)
{
    uint32_t scl_level = 0u;
    uint32_t sda_level = 0u;
    uint32_t scb_status = Cy_SCB_I2C_MasterGetStatus(PMBUS_CONTROLLER_HW,
                                                     &pmbus_i2c_ctx);

    read_i2c_pin_snapshot(&scl_level, &sda_level, NULL, NULL);
    if ((scl_level == 0u) || (sda_level == 0u))
    {
        printf("[PMBUS] WARN: skip SCB reset after %s because bus is not idle\n",
               reason);
        log_i2c_bus_snapshot("controller-reset-skip", scb_status);
        arm_bus_backoff(reason);
        return false;
    }

    printf("[PMBUS] WARN: resetting SCB after %s\n", reason);
    log_i2c_bus_snapshot("controller-reset-before", scb_status);
    Cy_SCB_I2C_Disable(PMBUS_CONTROLLER_HW, &pmbus_i2c_ctx);
    Cy_SCB_I2C_Enable(PMBUS_CONTROLLER_HW);
    log_i2c_bus_snapshot("controller-reset-after",
                         Cy_SCB_I2C_MasterGetStatus(PMBUS_CONTROLLER_HW,
                                                    &pmbus_i2c_ctx));

    bool reset_ok = (0u == (Cy_SCB_I2C_MasterGetStatus(PMBUS_CONTROLLER_HW,
                                                       &pmbus_i2c_ctx) &
                            CY_SCB_I2C_MASTER_BUSY));

    if (reset_ok)
    {
        /* D1-2: distinct event + metric for the controller-reset path */
        gateway_ipc_post_event(EVT_I2C_CONTROLLER_RESET, reason);
        metrics_inc_i2c_controller_resets();
        apply_recovery_settle_delay();
    }

    return reset_ok;
}

static void log_i2c_error(const char *op, const char *phase,
                          uint8_t addr_7bit, uint8_t cmd, uint8_t attempt,
                          uint8_t max_retries, pmbus_status_t result,
                          uint32_t scb_status)
{
    uint32_t scl_level;
    uint32_t sda_level;
    uint32_t scl_hsiom;
    uint32_t sda_hsiom;

    read_i2c_pin_snapshot(&scl_level, &sda_level, &scl_hsiom, &sda_hsiom);

    printf("[PMBUS] ERROR: %s %s addr=0x%02X cmd=0x%02X attempt=%u/%u "
           "status=%s scb=0x%08lX scl=%lu sda=%lu hsiom_scl=%lu "
           "hsiom_sda=%lu\n",
           op,
           phase,
           (unsigned)addr_7bit,
           (unsigned)cmd,
           (unsigned)(attempt + 1u),
           (unsigned)(max_retries + 1u),
           pmbus_status_str(result),
           (unsigned long)scb_status,
           (unsigned long)scl_level,
           (unsigned long)sda_level,
           (unsigned long)scl_hsiom,
           (unsigned long)sda_hsiom);
}

static void log_i2c_pdl_error(const char *op, const char *phase,
                              uint8_t addr_7bit, uint8_t cmd, uint8_t attempt,
                              uint8_t max_retries,
                              cy_en_scb_i2c_status_t pdl_st,
                              pmbus_status_t result)
{
    uint32_t scl_level;
    uint32_t sda_level;
    uint32_t scl_hsiom;
    uint32_t sda_hsiom;

    read_i2c_pin_snapshot(&scl_level, &sda_level, &scl_hsiom, &sda_hsiom);

    printf("[PMBUS] ERROR: %s %s addr=0x%02X cmd=0x%02X attempt=%u/%u "
           "status=%s pdl=0x%02lX scl=%lu sda=%lu hsiom_scl=%lu "
           "hsiom_sda=%lu\n",
           op,
           phase,
           (unsigned)addr_7bit,
           (unsigned)cmd,
           (unsigned)(attempt + 1u),
           (unsigned)(max_retries + 1u),
           pmbus_status_str(result),
           (unsigned long)pdl_st,
           (unsigned long)scl_level,
           (unsigned long)sda_level,
           (unsigned long)scl_hsiom,
           (unsigned long)sda_hsiom);
}

/*******************************************************************************
 * PEC (CRC-8) — SMBus polynomial 0x07
 ******************************************************************************/
uint8_t pmbus_crc8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0x00u;

    for (uint8_t i = 0u; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t bit = 0u; bit < 8u; bit++)
        {
            if (crc & 0x80u)
            {
                crc = (uint8_t)((crc << 1u) ^ 0x07u);
            }
            else
            {
                crc <<= 1u;
            }
        }
    }
    return crc;
}

/*******************************************************************************
 * Initialization
 ******************************************************************************/
pmbus_status_t pmbus_init(void)
{
    cy_en_scb_i2c_status_t status;

    if (pmbus_initialized)
    {
        return PMBUS_OK;
    }

    pmbus_transaction_timeout_ms = g_config.i2c.transaction_timeout_ms;
    pmbus_bus_backoff_until = 0u;

    /* 1. Initialize SCB3 as I2C master using Device Configurator config */
    status = Cy_SCB_I2C_Init(PMBUS_CONTROLLER_HW, &PMBUS_CONTROLLER_config,
                             &pmbus_i2c_ctx);
    if (CY_SCB_I2C_SUCCESS != status)
    {
        printf("[PMBUS] ERROR: Cy_SCB_I2C_Init failed (0x%02lX)\n",
               (unsigned long)status);
        return PMBUS_ERR_INIT;
    }

    /* 2. Configure the clock divider for SCB3.
     *    The Device Configurator assigns PCLK_SCB3_CLOCK to DIV_8_BIT[1].
     *    We set the divider value so the SCB clock matches the desired data rate.
     *
     *    For 100 kHz I2C with 100 MHz PERI clock:
     *      SCB clock should be ~1.6 MHz (16x oversampling at duty 16/16).
     *      Divider = ceil(100 MHz / 1.6 MHz) - 1 = 62
     *
     *    Cy_SCB_I2C_SetDataRate() calculates and returns the actual rate.
     */
    {
        /* Source clock for SCB3 peripheral divider: PERI clock (typically 100 MHz) */
        uint32_t peri_clk_hz = Cy_SysClk_ClkPeriGetFrequency();

        /* Compute divider for desired I2C oversampling.
         * For standard mode (100 kHz), high+low = 16+16 = 32 → SCB needs 3.2 MHz.
         * But SetDataRate handles internal calculation. We set divider to get
         * a reasonable SCB source clock and let SetDataRate fine-tune. */
        uint32_t desired_scb_clk;
        if (g_config.i2c.speed_hz <= 100000u)
        {
            desired_scb_clk = 1600000u;  /* 1.6 MHz for standard mode */
        }
        else
        {
            desired_scb_clk = 12800000u; /* 12.8 MHz for fast mode */
        }

        uint32_t divider_val = (peri_clk_hz + desired_scb_clk - 1u) / desired_scb_clk;
        if (divider_val > 0u)
        {
            divider_val -= 1u;  /* divider register is (N-1) */
        }

        Cy_SysClk_PeriphDisableDivider(PMBUS_CONTROLLER_CLK_HW,
                                        PMBUS_CONTROLLER_CLK_NUM);
        Cy_SysClk_PeriphSetDivider(PMBUS_CONTROLLER_CLK_HW,
                                   PMBUS_CONTROLLER_CLK_NUM, divider_val);
        Cy_SysClk_PeriphEnableDivider(PMBUS_CONTROLLER_CLK_HW,
                                      PMBUS_CONTROLLER_CLK_NUM);

        uint32_t actual_scb_clk = peri_clk_hz / (divider_val + 1u);
        uint32_t actual_rate = Cy_SCB_I2C_SetDataRate(PMBUS_CONTROLLER_HW,
                                                      g_config.i2c.speed_hz,
                                                      actual_scb_clk);

        printf("[PMBUS] I2C init: requested=%lu Hz, actual=%lu Hz, "
               "SCB_CLK=%lu Hz (div=%lu)\n",
               (unsigned long)g_config.i2c.speed_hz,
               (unsigned long)actual_rate,
               (unsigned long)actual_scb_clk,
               (unsigned long)(divider_val + 1u));
    }

    /* 3. Configure and enable the SCB3 interrupt */
    (void)Cy_SysInt_Init(&pmbus_scb3_irq_cfg, pmbus_scb3_isr);
    NVIC_EnableIRQ(pmbus_scb3_irq_cfg.intrSrc);

    /* 4. Enable the I2C block */
    Cy_SCB_I2C_Enable(PMBUS_CONTROLLER_HW);

    pmbus_initialized = true;

    printf("[PMBUS] Init OK: timeout=%lums retries=%u pec=%d recovery=%d\n",
           (unsigned long)g_config.i2c.transaction_timeout_ms,
           (unsigned)g_config.i2c.retries,
           (int)g_config.i2c.pec_enabled,
           (int)g_config.i2c.bus_recovery);

    return PMBUS_OK;
}

void pmbus_deinit(void)
{
    if (!pmbus_initialized) return;

    NVIC_DisableIRQ(pmbus_scb3_irq_cfg.intrSrc);
    Cy_SCB_I2C_Disable(PMBUS_CONTROLLER_HW, &pmbus_i2c_ctx);

    pmbus_initialized = false;
    pmbus_bus_backoff_until = 0u;
    printf("[PMBUS] De-initialized\n");
}

/*******************************************************************************
 * Bus Recovery — toggle SCL 9 times to release stuck SDA
 ******************************************************************************/
pmbus_status_t pmbus_bus_recovery(void)
{
    printf("[PMBUS] Bus recovery: toggling SCL 9 times\n");
    log_i2c_bus_snapshot("recovery-start",
                         Cy_SCB_I2C_MasterGetStatus(PMBUS_CONTROLLER_HW,
                                                    &pmbus_i2c_ctx));

    /*
     * Temporarily reconfigure SCL (P6_0) as GPIO output, toggle 9 times,
     * then restore to I2C function via HSIOM.
     *
     * IMPORTANT: The SCB must be disabled before we take over the pins
     * and re-enabled afterwards, otherwise the peripheral fights with our
     * GPIO writes and can generate spurious bus errors.
     */

    /* 1. Disable the I2C block so it releases the pins */
    Cy_SCB_I2C_Disable(PMBUS_CONTROLLER_HW, &pmbus_i2c_ctx);

    /* Save current HSIOM setting */
    en_hsiom_sel_t saved_hsiom = Cy_GPIO_GetHSIOM(CYBSP_I2C_SCL_PORT,
                                                   CYBSP_I2C_SCL_PIN);

    /* Switch SCL to GPIO mode — use open-drain (OD) drive to match I2C
     * electrical spec.  CY_GPIO_DM_OD_DRIVESLOW can only pull low; the
     * external pull-up drives the line high. */
    Cy_GPIO_SetHSIOM(CYBSP_I2C_SCL_PORT, CYBSP_I2C_SCL_PIN, HSIOM_SEL_GPIO);
    Cy_GPIO_SetDrivemode(CYBSP_I2C_SCL_PORT, CYBSP_I2C_SCL_PIN,
                         CY_GPIO_DM_OD_DRIVESLOW);

    for (int i = 0; i < 9; i++)
    {
        Cy_GPIO_Write(CYBSP_I2C_SCL_PORT, CYBSP_I2C_SCL_PIN, 0u);
        Cy_SysLib_DelayUs(5);
        Cy_GPIO_Write(CYBSP_I2C_SCL_PORT, CYBSP_I2C_SCL_PIN, 1u);
        Cy_SysLib_DelayUs(5);
    }

    /* Restore HSIOM — drive mode stays OD (correct for I2C) */
    Cy_GPIO_SetHSIOM(CYBSP_I2C_SCL_PORT, CYBSP_I2C_SCL_PIN, saved_hsiom);

    /* 2. Re-enable the I2C block */
    Cy_SCB_I2C_Enable(PMBUS_CONTROLLER_HW);
    log_i2c_bus_snapshot("recovery-end",
                         Cy_SCB_I2C_MasterGetStatus(PMBUS_CONTROLLER_HW,
                                                    &pmbus_i2c_ctx));

    /* Check if SDA is released (should be high) */
    uint32_t sda_val = Cy_GPIO_Read(CYBSP_I2C_SDA_PORT, CYBSP_I2C_SDA_PIN);
    if (sda_val == 0u)
    {
        printf("[PMBUS] Bus recovery FAILED: SDA still low\n");
        gateway_ipc_post_event(EVT_PMBUS_BUS_RECOVERY_FAIL, "SDA stuck low");
        return PMBUS_ERR_RECOVERY_FAIL;
    }

    printf("[PMBUS] Bus recovery OK\n");
    gateway_ipc_post_event(EVT_PMBUS_BUS_RECOVERY, NULL);
    metrics_inc_i2c_bus_recoveries();  /* D1-2: count successful bus recoveries */
    apply_recovery_settle_delay();
    return PMBUS_OK;
}

#ifdef PMBUS_TEST_HOOKS
bool pmbus_test_should_attempt_bus_recovery(pmbus_status_t status)
{
    return should_attempt_bus_recovery(status);
}

bool pmbus_test_should_attempt_controller_reset(pmbus_status_t status)
{
    return should_attempt_controller_reset(status);
}

bool pmbus_test_reset_controller_if_idle(const char *reason)
{
    return pmbus_reset_controller_if_idle(reason);
}
#endif

/*******************************************************************************
 * SMBus Read Word (with retries + optional PEC)
 *
 * Wire protocol:
 *   [S][addr<<1|W][cmd]  [Sr][addr<<1|R][data_low][data_high]  [P]
 *
 * With PEC:
 *   [S][addr<<1|W][cmd]  [Sr][addr<<1|R][data_low][data_high][pec]  [P]
 *
 * PEC is computed over: [addr<<1|W, cmd, addr<<1|R, data_low, data_high]
 ******************************************************************************/
pmbus_status_t pmbus_read_word(uint8_t addr_7bit, uint8_t cmd,
                              uint16_t *out_word, uint8_t *out_retries)
{
    if (!pmbus_initialized) return PMBUS_ERR_NOT_INIT;
    if (out_word == NULL)   return PMBUS_ERR_ARG;

    bool pec = g_config.i2c.pec_enabled;
    uint8_t max_retries = g_config.i2c.retries;
    pmbus_status_t result = PMBUS_ERR_BUS_FAULT;

    for (uint8_t attempt = 0; attempt <= max_retries; attempt++)
    {
        cy_en_scb_i2c_status_t pdl_st;
        uint32_t scb_status;
        wait_for_bus_backoff();

        /* --- Phase 1: Write the command byte (no STOP — xferPending) --- */
        uint8_t cmd_buf[1] = { cmd };
        cy_stc_scb_i2c_master_xfer_config_t wr_cfg = {
            .slaveAddress = addr_7bit,
            .buffer       = cmd_buf,
            .bufferSize   = 1u,
            .xferPending  = true,  /* No STOP — will follow with restart */
        };

        pdl_st = Cy_SCB_I2C_MasterWrite(PMBUS_CONTROLLER_HW, &wr_cfg,
                                        &pmbus_i2c_ctx);
        if (CY_SCB_I2C_SUCCESS != pdl_st)
        {
            result = map_pdl_status(pdl_st);
            log_i2c_pdl_error("read_word", "write-start",
                              addr_7bit, cmd, attempt, max_retries,
                              pdl_st, result);
            if (should_attempt_controller_reset(result))
            {
                (void)pmbus_reset_controller_if_idle("read_word write-start");
            }
            goto retry;
        }

        result = wait_for_completion(pmbus_transaction_timeout_ms);
        if (PMBUS_OK != result)
        {
            scb_status = Cy_SCB_I2C_MasterGetStatus(PMBUS_CONTROLLER_HW,
                                                    &pmbus_i2c_ctx);
            log_i2c_error("read_word", "write-complete",
                          addr_7bit, cmd, attempt, max_retries,
                          result, scb_status);
            goto retry;
        }

        /* --- Phase 2: Read 2 data bytes (+ optional PEC byte) --- */
        uint8_t read_len = pec ? 3u : 2u;
        uint8_t rd_buf[3] = {0};
        cy_stc_scb_i2c_master_xfer_config_t rd_cfg = {
            .slaveAddress = addr_7bit,
            .buffer       = rd_buf,
            .bufferSize   = read_len,
            .xferPending  = false,  /* Generate STOP */
        };

        pdl_st = Cy_SCB_I2C_MasterRead(PMBUS_CONTROLLER_HW, &rd_cfg,
                                       &pmbus_i2c_ctx);
        if (CY_SCB_I2C_SUCCESS != pdl_st)
        {
            result = map_pdl_status(pdl_st);
            log_i2c_pdl_error("read_word", "read-start",
                              addr_7bit, cmd, attempt, max_retries,
                              pdl_st, result);
            if (should_attempt_controller_reset(result))
            {
                (void)pmbus_reset_controller_if_idle("read_word read-start");
            }
            goto retry;
        }

        result = wait_for_completion(pmbus_transaction_timeout_ms);
        if (PMBUS_OK != result)
        {
            scb_status = Cy_SCB_I2C_MasterGetStatus(PMBUS_CONTROLLER_HW,
                                                    &pmbus_i2c_ctx);
            log_i2c_error("read_word", "read-complete",
                          addr_7bit, cmd, attempt, max_retries,
                          result, scb_status);
            goto retry;
        }

        /* --- Phase 3: Verify PEC if enabled --- */
        if (pec)
        {
            uint8_t pec_input[5] = {
                (uint8_t)(addr_7bit << 1u) | 0u,   /* addr + W */
                cmd,
                (uint8_t)(addr_7bit << 1u) | 1u,   /* addr + R */
                rd_buf[0],                          /* data low */
                rd_buf[1],                          /* data high */
            };
            uint8_t computed_pec = pmbus_crc8(pec_input, 5u);

            if (computed_pec != rd_buf[2])
            {
                result = PMBUS_ERR_PEC;
                log_i2c_error("read_word", "pec-check",
                              addr_7bit, cmd, attempt, max_retries,
                              result, 0u);
                goto retry;
            }
        }

        /* Success — assemble word (little-endian: low byte first) */
        *out_word = (uint16_t)rd_buf[0] | ((uint16_t)rd_buf[1] << 8u);
        if (out_retries != NULL) { *out_retries = attempt; }
        return PMBUS_OK;

retry:
        if (attempt < max_retries)
        {
            /* Only a bus-level SCB fault should trigger SCL-based recovery. */
            if (g_config.i2c.bus_recovery && should_attempt_bus_recovery(result))
            {
                printf("[PMBUS] WARN: invoking bus recovery after read_word "
                       "addr=0x%02X cmd=0x%02X attempt=%u/%u\n",
                       (unsigned)addr_7bit,
                       (unsigned)cmd,
                       (unsigned)(attempt + 1u),
                       (unsigned)(max_retries + 1u));
                pmbus_bus_recovery();
            }
            vTaskDelay(pdMS_TO_TICKS(1));  /* RTOS-friendly delay between retries */
        }
    }

    if (out_retries != NULL) { *out_retries = max_retries; }
    return result;
}

/*******************************************************************************
 * SMBus Read Byte (with retries + optional PEC)
 *
 * Wire protocol:
 *   [S][addr<<1|W][cmd]  [Sr][addr<<1|R][data]  [P]
 *
 * With PEC:
 *   [S][addr<<1|W][cmd]  [Sr][addr<<1|R][data][pec]  [P]
 *
 * PEC is computed over: [addr<<1|W, cmd, addr<<1|R, data]
 *
 * Used for single-byte PMBus commands such as VOUT_MODE (0x20).
 ******************************************************************************/
pmbus_status_t pmbus_read_byte(uint8_t addr_7bit, uint8_t cmd,
                              uint8_t *out_byte, uint8_t *out_retries)
{
    if (!pmbus_initialized) return PMBUS_ERR_NOT_INIT;
    if (out_byte == NULL)   return PMBUS_ERR_ARG;

    bool pec = g_config.i2c.pec_enabled;
    uint8_t max_retries = g_config.i2c.retries;
    pmbus_status_t result = PMBUS_ERR_BUS_FAULT;

    for (uint8_t attempt = 0; attempt <= max_retries; attempt++)
    {
        cy_en_scb_i2c_status_t pdl_st;
        uint32_t scb_status;
        wait_for_bus_backoff();

        /* --- Phase 1: Write the command byte (no STOP — xferPending) --- */
        uint8_t cmd_buf[1] = { cmd };
        cy_stc_scb_i2c_master_xfer_config_t wr_cfg = {
            .slaveAddress = addr_7bit,
            .buffer       = cmd_buf,
            .bufferSize   = 1u,
            .xferPending  = true,  /* No STOP — will follow with restart */
        };

        pdl_st = Cy_SCB_I2C_MasterWrite(PMBUS_CONTROLLER_HW, &wr_cfg,
                                        &pmbus_i2c_ctx);
        if (CY_SCB_I2C_SUCCESS != pdl_st)
        {
            result = map_pdl_status(pdl_st);
            log_i2c_pdl_error("read_byte", "write-start",
                              addr_7bit, cmd, attempt, max_retries,
                              pdl_st, result);
            if (should_attempt_controller_reset(result))
            {
                (void)pmbus_reset_controller_if_idle("read_byte write-start");
            }
            goto retry_byte;
        }

        result = wait_for_completion(pmbus_transaction_timeout_ms);
        if (PMBUS_OK != result)
        {
            scb_status = Cy_SCB_I2C_MasterGetStatus(PMBUS_CONTROLLER_HW,
                                                    &pmbus_i2c_ctx);
            log_i2c_error("read_byte", "write-complete",
                          addr_7bit, cmd, attempt, max_retries,
                          result, scb_status);
            goto retry_byte;
        }

        /* --- Phase 2: Read 1 data byte (+ optional PEC byte) --- */
        uint8_t read_len = pec ? 2u : 1u;
        uint8_t rd_buf[2] = {0};
        cy_stc_scb_i2c_master_xfer_config_t rd_cfg = {
            .slaveAddress = addr_7bit,
            .buffer       = rd_buf,
            .bufferSize   = read_len,
            .xferPending  = false,  /* Generate STOP */
        };

        pdl_st = Cy_SCB_I2C_MasterRead(PMBUS_CONTROLLER_HW, &rd_cfg,
                                       &pmbus_i2c_ctx);
        if (CY_SCB_I2C_SUCCESS != pdl_st)
        {
            result = map_pdl_status(pdl_st);
            log_i2c_pdl_error("read_byte", "read-start",
                              addr_7bit, cmd, attempt, max_retries,
                              pdl_st, result);
            if (should_attempt_controller_reset(result))
            {
                (void)pmbus_reset_controller_if_idle("read_byte read-start");
            }
            goto retry_byte;
        }

        result = wait_for_completion(pmbus_transaction_timeout_ms);
        if (PMBUS_OK != result)
        {
            scb_status = Cy_SCB_I2C_MasterGetStatus(PMBUS_CONTROLLER_HW,
                                                    &pmbus_i2c_ctx);
            log_i2c_error("read_byte", "read-complete",
                          addr_7bit, cmd, attempt, max_retries,
                          result, scb_status);
            goto retry_byte;
        }

        /* --- Phase 3: Verify PEC if enabled --- */
        if (pec)
        {
            uint8_t pec_input[4] = {
                (uint8_t)(addr_7bit << 1u) | 0u,   /* addr + W */
                cmd,
                (uint8_t)(addr_7bit << 1u) | 1u,   /* addr + R */
                rd_buf[0],                          /* data byte */
            };
            uint8_t computed_pec = pmbus_crc8(pec_input, 4u);

            if (computed_pec != rd_buf[1])
            {
                result = PMBUS_ERR_PEC;
                log_i2c_error("read_byte", "pec-check",
                              addr_7bit, cmd, attempt, max_retries,
                              result, 0u);
                goto retry_byte;
            }
        }

        /* Success */
        *out_byte = rd_buf[0];
        if (out_retries != NULL) { *out_retries = attempt; }
        return PMBUS_OK;

retry_byte:
        if (attempt < max_retries)
        {
            if (g_config.i2c.bus_recovery && should_attempt_bus_recovery(result))
            {
                printf("[PMBUS] WARN: invoking bus recovery after read_byte "
                       "addr=0x%02X cmd=0x%02X attempt=%u/%u\n",
                       (unsigned)addr_7bit,
                       (unsigned)cmd,
                       (unsigned)(attempt + 1u),
                       (unsigned)(max_retries + 1u));
                pmbus_bus_recovery();
            }
            vTaskDelay(pdMS_TO_TICKS(1));  /* RTOS-friendly delay between retries */
        }
    }

    if (out_retries != NULL) { *out_retries = max_retries; }
    return result;
}

/*******************************************************************************
 * SMBus Send Byte (no data, just command code)
 *
 * Wire protocol:
 *   [S][addr<<1|W][cmd][P]
 ******************************************************************************/
pmbus_status_t pmbus_send_byte(uint8_t addr_7bit, uint8_t cmd)
{
    if (!pmbus_initialized) return PMBUS_ERR_NOT_INIT;

    uint8_t max_retries = g_config.i2c.retries;
    pmbus_status_t result = PMBUS_ERR_BUS_FAULT;

    for (uint8_t attempt = 0; attempt <= max_retries; attempt++)
    {
        cy_en_scb_i2c_status_t pdl_st;
        uint32_t scb_status;
        wait_for_bus_backoff();

        uint8_t tx_buf[1] = { cmd };
        cy_stc_scb_i2c_master_xfer_config_t wr_cfg = {
            .slaveAddress = addr_7bit,
            .buffer       = tx_buf,
            .bufferSize   = 1u,
            .xferPending  = false,
        };

        pdl_st = Cy_SCB_I2C_MasterWrite(PMBUS_CONTROLLER_HW, &wr_cfg,
                                        &pmbus_i2c_ctx);
        if (CY_SCB_I2C_SUCCESS != pdl_st)
        {
            result = map_pdl_status(pdl_st);
            log_i2c_pdl_error("send_byte", "write-start",
                              addr_7bit, cmd, attempt, max_retries,
                              pdl_st, result);
            if (should_attempt_controller_reset(result))
            {
                (void)pmbus_reset_controller_if_idle("send_byte write-start");
            }
            goto send_retry;
        }

        result = wait_for_completion(pmbus_transaction_timeout_ms);
        if (PMBUS_OK == result)
        {
            return PMBUS_OK;
        }

        scb_status = Cy_SCB_I2C_MasterGetStatus(PMBUS_CONTROLLER_HW,
                                                &pmbus_i2c_ctx);
        log_i2c_error("send_byte", "write-complete",
                      addr_7bit, cmd, attempt, max_retries,
                      result, scb_status);

send_retry:
        if (attempt < max_retries)
        {
            if (g_config.i2c.bus_recovery && should_attempt_bus_recovery(result))
            {
                printf("[PMBUS] WARN: invoking bus recovery after send_byte "
                       "addr=0x%02X cmd=0x%02X attempt=%u/%u\n",
                       (unsigned)addr_7bit,
                       (unsigned)cmd,
                       (unsigned)(attempt + 1u),
                       (unsigned)(max_retries + 1u));
                pmbus_bus_recovery();
            }
            vTaskDelay(pdMS_TO_TICKS(1));  /* RTOS-friendly delay between retries */
        }
    }

    return result;
}

/*******************************************************************************
 * ARA (Alert Response Address) — D2c-1
 *
 * Wire protocol:
 *   [S][0x18|R][data_byte][P]   (bare 1-byte read, no command prefix)
 *
 * Contract:
 *   - No command byte write prefix (unlike normal PMBus Read Word)
 *   - No PEC — ARA is outside the PMBus command layer
 *   - No retries — single attempt
 *   - No bus recovery on NACK
 *   - No normal PMBus counter pollution on NACK
 *   - NACK = normal "no device asserting" exit
 ******************************************************************************/
pmbus_status_t pmbus_ara_read(uint8_t *out_addr_7bit)
{
    if (!pmbus_initialized) return PMBUS_ERR_NOT_INIT;
    if (out_addr_7bit == NULL) return PMBUS_ERR_ARG;

    uint8_t rd_buf[1] = { 0u };

    if (!wait_for_master_not_busy(pmbus_transaction_timeout_ms))
    {
        return PMBUS_ERR_TIMEOUT;
    }

    cy_stc_scb_i2c_master_xfer_config_t rd_cfg = {
        .slaveAddress = PMBUS_ARA_ADDR_7BIT,
        .buffer       = rd_buf,
        .bufferSize   = 1u,
        .xferPending  = false,
    };

    cy_en_scb_i2c_status_t pdl_st =
        Cy_SCB_I2C_MasterRead(PMBUS_CONTROLLER_HW, &rd_cfg, &pmbus_i2c_ctx);

    if (CY_SCB_I2C_SUCCESS != pdl_st)
    {
        return map_pdl_status(pdl_st);
    }

    pmbus_status_t st = wait_for_completion(pmbus_transaction_timeout_ms);

    if (st == PMBUS_OK)
    {
        *out_addr_7bit = rd_buf[0] >> 1u;
    }

    /* NACK / TIMEOUT / BUS_FAULT: return as-is.
     * No retries, no recovery, no counter increments. */
    return st;
}
