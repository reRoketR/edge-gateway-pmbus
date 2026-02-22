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

/* PDL I2C driver */
#include "cy_scb_i2c.h"
#include "cy_sysint.h"
#include "cy_sysclk.h"

/* Device Configurator generated: PMBUS_CONTROLLER on SCB3 */
#include "cycfg_peripherals.h"

/* For GPIO bus-recovery */
#include "cy_gpio.h"
#include "cycfg_pins.h"
#include "cy_syslib.h"

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
static uint32_t pmbus_timeout_ms;

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

/**
 * @brief Wait for a PDL I2C master transfer to complete.
 *
 * Polls Cy_SCB_I2C_MasterGetStatus() until:
 *   - transfer completes (no CY_SCB_I2C_MASTER_BUSY), or
 *   - timeout_ms elapses.
 *
 * @return PMBUS_OK, PMBUS_ERR_NACK, PMBUS_ERR_TIMEOUT, or PMBUS_ERR_BUS.
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
                return PMBUS_ERR_BUS;
            }
            if (status & CY_SCB_I2C_MASTER_BUS_ERR)
            {
                return PMBUS_ERR_BUS;
            }
            return PMBUS_OK;
        }

        Cy_SysLib_Delay(1u);
        elapsed++;
    }

    /* Timeout — abort the pending transfer */
    Cy_SCB_I2C_MasterAbortWrite(PMBUS_CONTROLLER_HW, &pmbus_i2c_ctx);
    Cy_SCB_I2C_MasterAbortRead(PMBUS_CONTROLLER_HW, &pmbus_i2c_ctx);
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
        case CY_SCB_I2C_MASTER_NOT_READY: return PMBUS_ERR_BUS;
        case CY_SCB_I2C_BAD_PARAM:        return PMBUS_ERR_ARG;
        default:                          return PMBUS_ERR_BUS;
    }
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

    pmbus_timeout_ms = g_config.i2c.timeout_ms;

    /* 1. Initialize SCB3 as I2C master using Device Configurator config */
    status = Cy_SCB_I2C_Init(PMBUS_CONTROLLER_HW, &PMBUS_CONTROLLER_config,
                             &pmbus_i2c_ctx);
    if (CY_SCB_I2C_SUCCESS != status)
    {
        printf("[PMBUS] ERROR: Cy_SCB_I2C_Init failed (0x%02lX)\n",
               (unsigned long)status);
        return PMBUS_ERR_BUS;
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

        Cy_SysClk_PeriphDisableDivider(CY_SYSCLK_DIV_8_BIT, 1u);
        Cy_SysClk_PeriphSetDivider(CY_SYSCLK_DIV_8_BIT, 1u, divider_val);
        Cy_SysClk_PeriphEnableDivider(CY_SYSCLK_DIV_8_BIT, 1u);

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
           (unsigned long)g_config.i2c.timeout_ms,
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
    printf("[PMBUS] De-initialized\n");
}

/*******************************************************************************
 * Bus Recovery — toggle SCL 9 times to release stuck SDA
 ******************************************************************************/
pmbus_status_t pmbus_bus_recovery(void)
{
    printf("[PMBUS] Bus recovery: toggling SCL 9 times\n");

    /*
     * Temporarily reconfigure SCL (P6_0) as GPIO output, toggle 9 times,
     * then restore to I2C function via HSIOM.
     */

    /* Save current HSIOM setting */
    en_hsiom_sel_t saved_hsiom = Cy_GPIO_GetHSIOM(CYBSP_I2C_SCL_PORT,
                                                   CYBSP_I2C_SCL_PIN);

    /* Switch SCL to GPIO mode */
    Cy_GPIO_SetHSIOM(CYBSP_I2C_SCL_PORT, CYBSP_I2C_SCL_PIN, HSIOM_SEL_GPIO);
    Cy_GPIO_SetDrivemode(CYBSP_I2C_SCL_PORT, CYBSP_I2C_SCL_PIN,
                         CY_GPIO_DM_STRONG_IN_OFF);

    for (int i = 0; i < 9; i++)
    {
        Cy_GPIO_Write(CYBSP_I2C_SCL_PORT, CYBSP_I2C_SCL_PIN, 0u);
        Cy_SysLib_DelayUs(5);
        Cy_GPIO_Write(CYBSP_I2C_SCL_PORT, CYBSP_I2C_SCL_PIN, 1u);
        Cy_SysLib_DelayUs(5);
    }

    /* Restore HSIOM and drive mode */
    Cy_GPIO_SetDrivemode(CYBSP_I2C_SCL_PORT, CYBSP_I2C_SCL_PIN,
                         CY_GPIO_DM_OD_DRIVESLOW);
    Cy_GPIO_SetHSIOM(CYBSP_I2C_SCL_PORT, CYBSP_I2C_SCL_PIN, saved_hsiom);

    /* Check if SDA is released (should be high) */
    uint32_t sda_val = Cy_GPIO_Read(CYBSP_I2C_SDA_PORT, CYBSP_I2C_SDA_PIN);
    if (sda_val == 0u)
    {
        printf("[PMBUS] Bus recovery FAILED: SDA still low\n");
        return PMBUS_ERR_BUS;
    }

    printf("[PMBUS] Bus recovery OK\n");
    return PMBUS_OK;
}

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
pmbus_status_t pmbus_read_word(uint8_t addr_7bit, uint8_t cmd, uint16_t *out_word)
{
    if (!pmbus_initialized) return PMBUS_ERR_NOT_INIT;
    if (out_word == NULL)   return PMBUS_ERR_ARG;

    bool pec = g_config.i2c.pec_enabled;
    uint8_t max_retries = g_config.i2c.retries;
    pmbus_status_t result = PMBUS_ERR_BUS;

    for (uint8_t attempt = 0; attempt <= max_retries; attempt++)
    {
        cy_en_scb_i2c_status_t pdl_st;

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
            goto retry;
        }

        result = wait_for_completion(pmbus_timeout_ms);
        if (PMBUS_OK != result)
        {
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
            goto retry;
        }

        result = wait_for_completion(pmbus_timeout_ms);
        if (PMBUS_OK != result)
        {
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
                goto retry;
            }
        }

        /* Success — assemble word (little-endian: low byte first) */
        *out_word = (uint16_t)rd_buf[0] | ((uint16_t)rd_buf[1] << 8u);
        return PMBUS_OK;

retry:
        if (attempt < max_retries)
        {
            /* If bus error and recovery enabled, try to recover */
            if (result == PMBUS_ERR_BUS && g_config.i2c.bus_recovery)
            {
                pmbus_bus_recovery();
            }
            Cy_SysLib_Delay(1u);  /* Brief delay between retries */
        }
    }

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
pmbus_status_t pmbus_read_byte(uint8_t addr_7bit, uint8_t cmd, uint8_t *out_byte)
{
    if (!pmbus_initialized) return PMBUS_ERR_NOT_INIT;
    if (out_byte == NULL)   return PMBUS_ERR_ARG;

    bool pec = g_config.i2c.pec_enabled;
    uint8_t max_retries = g_config.i2c.retries;
    pmbus_status_t result = PMBUS_ERR_BUS;

    for (uint8_t attempt = 0; attempt <= max_retries; attempt++)
    {
        cy_en_scb_i2c_status_t pdl_st;

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
            goto retry_byte;
        }

        result = wait_for_completion(pmbus_timeout_ms);
        if (PMBUS_OK != result)
        {
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
            goto retry_byte;
        }

        result = wait_for_completion(pmbus_timeout_ms);
        if (PMBUS_OK != result)
        {
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
                goto retry_byte;
            }
        }

        /* Success */
        *out_byte = rd_buf[0];
        return PMBUS_OK;

retry_byte:
        if (attempt < max_retries)
        {
            if (result == PMBUS_ERR_BUS && g_config.i2c.bus_recovery)
            {
                pmbus_bus_recovery();
            }
            Cy_SysLib_Delay(1u);
        }
    }

    return result;
}

/*******************************************************************************
 * SMBus Read Block
 *
 * Wire protocol:
 *   [S][addr<<1|W][cmd]  [Sr][addr<<1|R][byte_count][data0..dataN-1]  [P]
 *
 * With PEC, the last received byte is the PEC byte and is not part of data.
 * PEC is computed over: [addr<<1|W, cmd, addr<<1|R, byte_count, data0..dataN-1]
 ******************************************************************************/
pmbus_status_t pmbus_read_block(uint8_t addr_7bit, uint8_t cmd,
                                uint8_t *buf, uint8_t buf_size,
                                uint8_t *out_len)
{
    if (!pmbus_initialized) return PMBUS_ERR_NOT_INIT;
    if (buf == NULL || out_len == NULL || buf_size == 0u) return PMBUS_ERR_ARG;

    bool pec = g_config.i2c.pec_enabled;
    uint8_t max_retries = g_config.i2c.retries;
    pmbus_status_t result = PMBUS_ERR_BUS;

    /*
     * SMBus block read: the target sends byte_count first, then data bytes.
     * Max block size per SMBus spec is 32 bytes + 1 byte_count + optional PEC.
     * We use a fixed buffer of 34 bytes to accommodate worst case.
     */
    uint8_t raw_buf[34] = {0};

    for (uint8_t attempt = 0; attempt <= max_retries; attempt++)
    {
        cy_en_scb_i2c_status_t pdl_st;

        /* --- Phase 1: Write command byte (xferPending) --- */
        uint8_t cmd_buf[1] = { cmd };
        cy_stc_scb_i2c_master_xfer_config_t wr_cfg = {
            .slaveAddress = addr_7bit,
            .buffer       = cmd_buf,
            .bufferSize   = 1u,
            .xferPending  = true,
        };

        pdl_st = Cy_SCB_I2C_MasterWrite(PMBUS_CONTROLLER_HW, &wr_cfg,
                                        &pmbus_i2c_ctx);
        if (CY_SCB_I2C_SUCCESS != pdl_st)
        {
            result = map_pdl_status(pdl_st);
            goto block_retry;
        }

        result = wait_for_completion(pmbus_timeout_ms);
        if (PMBUS_OK != result) goto block_retry;

        /*
         * --- Phase 2: Read using low-level API to handle byte_count --- 
         *
         * We first read 1 byte to get byte_count, then read the remaining
         * data bytes. However, since we need to keep the bus held between
         * reads, we'll use a conservative approach: read up to max expected
         * bytes (buf_size + 1 for byte_count + 1 for PEC).
         */
        {
            uint8_t max_read = buf_size + 1u;  /* +1 for byte_count */
            if (pec) max_read += 1u;           /* +1 for PEC */
            if (max_read > sizeof(raw_buf)) max_read = sizeof(raw_buf);

            cy_stc_scb_i2c_master_xfer_config_t rd_cfg = {
                .slaveAddress = addr_7bit,
                .buffer       = raw_buf,
                .bufferSize   = max_read,
                .xferPending  = false,
            };

            pdl_st = Cy_SCB_I2C_MasterRead(PMBUS_CONTROLLER_HW, &rd_cfg,
                                           &pmbus_i2c_ctx);
            if (CY_SCB_I2C_SUCCESS != pdl_st)
            {
                result = map_pdl_status(pdl_st);
                goto block_retry;
            }

            result = wait_for_completion(pmbus_timeout_ms);
            if (PMBUS_OK != result) goto block_retry;
        }

        /* Parse: raw_buf[0] = byte_count, raw_buf[1..N] = data */
        uint8_t byte_count = raw_buf[0];
        if (byte_count > buf_size)
        {
            result = PMBUS_ERR_ARG;
            goto block_retry;
        }

        /* Verify PEC if enabled */
        if (pec)
        {
            /* PEC covers: addr+W, cmd, addr+R, byte_count, data[0..N-1] */
            uint8_t pec_buf[37];  /* 3 header + 1 byte_count + 32 data max + 1 */
            uint8_t pec_len = 0;

            pec_buf[pec_len++] = (uint8_t)(addr_7bit << 1u) | 0u;
            pec_buf[pec_len++] = cmd;
            pec_buf[pec_len++] = (uint8_t)(addr_7bit << 1u) | 1u;
            pec_buf[pec_len++] = byte_count;
            memcpy(&pec_buf[pec_len], &raw_buf[1], byte_count);
            pec_len += byte_count;

            uint8_t computed = pmbus_crc8(pec_buf, pec_len);
            uint8_t received = raw_buf[1u + byte_count];  /* PEC byte follows data */

            if (computed != received)
            {
                result = PMBUS_ERR_PEC;
                goto block_retry;
            }
        }

        /* Copy data to output buffer */
        memcpy(buf, &raw_buf[1], byte_count);
        *out_len = byte_count;
        return PMBUS_OK;

block_retry:
        if (attempt < max_retries)
        {
            if (result == PMBUS_ERR_BUS && g_config.i2c.bus_recovery)
            {
                pmbus_bus_recovery();
            }
            Cy_SysLib_Delay(1u);
        }
    }

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
    pmbus_status_t result = PMBUS_ERR_BUS;

    for (uint8_t attempt = 0; attempt <= max_retries; attempt++)
    {
        cy_en_scb_i2c_status_t pdl_st;

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
            goto send_retry;
        }

        result = wait_for_completion(pmbus_timeout_ms);
        if (PMBUS_OK == result)
        {
            return PMBUS_OK;
        }

send_retry:
        if (attempt < max_retries)
        {
            if (result == PMBUS_ERR_BUS && g_config.i2c.bus_recovery)
            {
                pmbus_bus_recovery();
            }
            Cy_SysLib_Delay(1u);
        }
    }

    return result;
}
