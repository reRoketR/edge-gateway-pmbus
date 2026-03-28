/* Stub cy_scb_i2c.h for host-side unit tests. */
#ifndef CY_SCB_I2C_STUB_H
#define CY_SCB_I2C_STUB_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    CY_SCB_I2C_SUCCESS = 0,
    CY_SCB_I2C_MASTER_NOT_READY = 1,
    CY_SCB_I2C_BAD_PARAM = 2,
} cy_en_scb_i2c_status_t;

typedef struct {
    uint32_t dummy;
} cy_stc_scb_i2c_context_t;

typedef struct {
    uint16_t slaveAddress;
    uint8_t *buffer;
    uint32_t bufferSize;
    bool xferPending;
} cy_stc_scb_i2c_master_xfer_config_t;

#define CY_SCB_I2C_MASTER_BUSY      (1u << 0)
#define CY_SCB_I2C_MASTER_ADDR_NAK  (1u << 1)
#define CY_SCB_I2C_MASTER_DATA_NAK  (1u << 2)
#define CY_SCB_I2C_MASTER_ARB_LOST  (1u << 3)
#define CY_SCB_I2C_MASTER_BUS_ERR   (1u << 4)

cy_en_scb_i2c_status_t Cy_SCB_I2C_Init(void *base, const void *config,
                                        cy_stc_scb_i2c_context_t *context);
uint32_t Cy_SCB_I2C_SetDataRate(void *base, uint32_t dataRate, uint32_t scbClock);
void Cy_SCB_I2C_Enable(void *base);
void Cy_SCB_I2C_Disable(void *base, cy_stc_scb_i2c_context_t *context);
void Cy_SCB_I2C_Interrupt(void *base, cy_stc_scb_i2c_context_t *context);
uint32_t Cy_SCB_I2C_MasterGetStatus(void *base, cy_stc_scb_i2c_context_t *context);
cy_en_scb_i2c_status_t Cy_SCB_I2C_MasterWrite(
    void *base,
    cy_stc_scb_i2c_master_xfer_config_t *config,
    cy_stc_scb_i2c_context_t *context);
cy_en_scb_i2c_status_t Cy_SCB_I2C_MasterRead(
    void *base,
    cy_stc_scb_i2c_master_xfer_config_t *config,
    cy_stc_scb_i2c_context_t *context);
void Cy_SCB_I2C_MasterAbortWrite(void *base, cy_stc_scb_i2c_context_t *context);
void Cy_SCB_I2C_MasterAbortRead(void *base, cy_stc_scb_i2c_context_t *context);

#endif /* CY_SCB_I2C_STUB_H */
