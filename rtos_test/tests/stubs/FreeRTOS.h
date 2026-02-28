/* Stub FreeRTOS.h for host-side unit tests (single-threaded). */
#ifndef FREERTOS_STUB_H
#define FREERTOS_STUB_H

#include <stdint.h>

typedef uint32_t  TickType_t;
typedef uint32_t  BaseType_t;
typedef void*     TaskHandle_t;

#define pdTRUE   1
#define pdFALSE  0
#define pdPASS   pdTRUE

#define portMAX_DELAY  0xFFFFFFFFUL

#define taskENTER_CRITICAL()  do {} while(0)
#define taskEXIT_CRITICAL()   do {} while(0)

#endif /* FREERTOS_STUB_H */
