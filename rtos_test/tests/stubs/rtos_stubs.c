/*******************************************************************************
 * File Name:   rtos_stubs.c
 *
 * Description: FreeRTOS queue, task, and wallclock stubs for host-side
 *              integration tests. Single-threaded, deterministic.
 ******************************************************************************/

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/*******************************************************************************
 * Queue model — max 8 concurrent queues, FIFO, non-blocking only
 ******************************************************************************/

#define MAX_QUEUES 8u

typedef struct {
    bool     in_use;
    uint8_t *buf;
    uint32_t item_size;
    uint32_t capacity;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} stub_queue_t;

static stub_queue_t s_queues[MAX_QUEUES];

QueueHandle_t xQueueCreate(uint32_t length, uint32_t item_size)
{
    for (uint32_t i = 0; i < MAX_QUEUES; i++)
    {
        if (!s_queues[i].in_use)
        {
            s_queues[i].in_use    = true;
            s_queues[i].item_size = item_size;
            s_queues[i].capacity  = length;
            s_queues[i].head      = 0;
            s_queues[i].tail      = 0;
            s_queues[i].count     = 0;
            s_queues[i].buf       = (uint8_t *)malloc((size_t)length * item_size);
            if (s_queues[i].buf == NULL)
            {
                s_queues[i].in_use = false;
                return NULL;
            }
            return (QueueHandle_t)&s_queues[i];
        }
    }
    return NULL;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    (void)wait;
    if (q == NULL || item == NULL) return pdFALSE;

    stub_queue_t *sq = (stub_queue_t *)q;
    if (sq->count >= sq->capacity) return pdFALSE;

    memcpy(&sq->buf[sq->head * sq->item_size], item, sq->item_size);
    sq->head = (sq->head + 1u) % sq->capacity;
    sq->count++;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *buf, TickType_t wait)
{
    (void)wait;
    if (q == NULL || buf == NULL) return pdFALSE;

    stub_queue_t *sq = (stub_queue_t *)q;
    if (sq->count == 0u) return pdFALSE;

    memcpy(buf, &sq->buf[sq->tail * sq->item_size], sq->item_size);
    sq->tail = (sq->tail + 1u) % sq->capacity;
    sq->count--;
    return pdTRUE;
}

uint32_t uxQueueMessagesWaiting(QueueHandle_t q)
{
    if (q == NULL) return 0u;
    return ((stub_queue_t *)q)->count;
}

void test_queue_reset_all(void)
{
    for (uint32_t i = 0; i < MAX_QUEUES; i++)
    {
        if (s_queues[i].in_use && s_queues[i].buf != NULL)
        {
            free(s_queues[i].buf);
        }
        memset(&s_queues[i], 0, sizeof(stub_queue_t));
    }
}

/*******************************************************************************
 * Task stubs
 ******************************************************************************/

static TickType_t s_tick = 0u;

void test_set_tick(TickType_t t)
{
    s_tick = t;
}

TickType_t xTaskGetTickCount(void)
{
    return s_tick;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return (TaskHandle_t)1;
}

BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
    (void)task;
    return pdPASS;
}

uint32_t ulTaskNotifyTake(BaseType_t clear_count_on_exit, TickType_t ticks_to_wait)
{
    (void)clear_count_on_exit;
    (void)ticks_to_wait;
    return 0u;
}

void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
}

int xTaskGetSchedulerState(void)
{
    return taskSCHEDULER_RUNNING;
}

/*******************************************************************************
 * Wallclock stubs
 ******************************************************************************/

static uint64_t s_wall_ms = 1000000u;

void test_set_wall_ms(uint64_t ms)
{
    s_wall_ms = ms;
}

uint64_t wallclock_now_ms(void)
{
    return s_wall_ms;
}

bool wallclock_is_synced(void)
{
    return true;
}

void wallclock_sntp_init(void)
{
    /* no-op */
}

/* [] END OF FILE */
