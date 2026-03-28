/* Stub cycfg_peripherals.h for host-side unit tests. */
#ifndef CYCFG_PERIPHERALS_STUB_H
#define CYCFG_PERIPHERALS_STUB_H

extern int mock_pmbus_controller_hw;
extern const int PMBUS_CONTROLLER_config;

#define PMBUS_CONTROLLER_HW  ((void *)&mock_pmbus_controller_hw)
#define PMBUS_CONTROLLER_IRQ 5

#endif /* CYCFG_PERIPHERALS_STUB_H */
