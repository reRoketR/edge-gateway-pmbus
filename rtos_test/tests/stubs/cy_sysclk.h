/* Stub cy_sysclk.h for host-side unit tests. */
#ifndef CY_SYSCLK_STUB_H
#define CY_SYSCLK_STUB_H

#include <stdint.h>

uint32_t Cy_SysClk_ClkPeriGetFrequency(void);
void Cy_SysClk_PeriphDisableDivider(uint32_t hw, uint32_t num);
void Cy_SysClk_PeriphSetDivider(uint32_t hw, uint32_t num, uint32_t val);
void Cy_SysClk_PeriphEnableDivider(uint32_t hw, uint32_t num);

#endif /* CY_SYSCLK_STUB_H */
