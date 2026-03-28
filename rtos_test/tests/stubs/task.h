/* Stub task.h for host-side unit tests. */
#ifndef TASK_STUB_H
#define TASK_STUB_H

#include "FreeRTOS.h"

TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t ticks);

#endif /* TASK_STUB_H */
