/* Stub cy_sysint.h for host-side unit tests. */
#ifndef CY_SYSINT_STUB_H
#define CY_SYSINT_STUB_H

#include <stdint.h>

typedef struct {
    int intrSrc;
    uint32_t intrPriority;
} cy_stc_sysint_t;

int Cy_SysInt_Init(const cy_stc_sysint_t *cfg, void (*isr)(void));
void NVIC_EnableIRQ(int irqn);
void NVIC_DisableIRQ(int irqn);

#endif /* CY_SYSINT_STUB_H */
