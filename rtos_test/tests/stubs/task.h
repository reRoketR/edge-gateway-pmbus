/* Stub task.h for host-side unit tests. */
#ifndef TASK_STUB_H
#define TASK_STUB_H

#include "FreeRTOS.h"

static inline TickType_t xTaskGetTickCount(void) { return 0; }

#endif /* TASK_STUB_H */
