/* Stub task.h for host-side unit tests. */
#ifndef TASK_STUB_H
#define TASK_STUB_H

#include "FreeRTOS.h"

TickType_t xTaskGetTickCount(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void vTaskDelay(TickType_t ticks);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear_count_on_exit, TickType_t ticks_to_wait);

#endif /* TASK_STUB_H */
