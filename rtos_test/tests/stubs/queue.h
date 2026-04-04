/* Stub queue.h for host-side unit tests. */
#ifndef QUEUE_STUB_H
#define QUEUE_STUB_H

#include "FreeRTOS.h"

typedef void *QueueHandle_t;

QueueHandle_t xQueueCreate(uint32_t length, uint32_t item_size);
#define xQueueGenericCreate(l, s, t) xQueueCreate((l), (s))
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t q, void *buf, TickType_t wait);
uint32_t   uxQueueMessagesWaiting(QueueHandle_t q);

/* Test helper: free all queue backing stores */
void test_queue_reset_all(void);

#endif /* QUEUE_STUB_H */
