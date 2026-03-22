/******************************************************************************
* File Name:   FreeRTOSConfig.h
*
* Description: FreeRTOS configuration for PMBus-MQTT Gateway.
*              Based on Infineon Wi-Fi MQTT Client example (CE229889).
*
*******************************************************************************
* Copyright 2020-2025, Cypress Semiconductor Corporation (an Infineon company)
* SPDX-License-Identifier: Apache-2.0
*******************************************************************************/

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include "cy_utils.h"

/* Get the low power configuration parameters from
 * the ModusToolbox Device Configurator GeneratedSource:
 * CY_CFG_PWR_SYS_IDLE_MODE     - System Idle Power Mode
 * CY_CFG_PWR_DEEPSLEEP_LATENCY - Deep Sleep Latency (ms)
 */
#include "cycfg_system.h"

#define configUSE_PREEMPTION                    1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  0
#define configCPU_CLOCK_HZ                      SystemCoreClock
#define configTICK_RATE_HZ                      1000u
#define configMAX_PRIORITIES                    7
#define configMINIMAL_STACK_SIZE                128
#define configMAX_TASK_NAME_LEN                 16
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_TASK_NOTIFICATIONS            1
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               10
#define configUSE_QUEUE_SETS                    0
#define configUSE_TIME_SLICING                  1
#define configENABLE_BACKWARD_COMPATIBILITY     0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS  16

/* Memory allocation related definitions. */
#define configSUPPORT_STATIC_ALLOCATION         1
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configTOTAL_HEAP_SIZE                   (200 * 1024)
#define configAPPLICATION_ALLOCATED_HEAP        0

/* Hook function related definitions. */
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_MALLOC_FAILED_HOOK            1
#define configUSE_DAEMON_TASK_STARTUP_HOOK      0

/* Run time and task stats gathering related definitions. */
#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_TRACE_FACILITY                1
#define configUSE_STATS_FORMATTING_FUNCTIONS    0

/* Co-routine related definitions. */
#define configUSE_CO_ROUTINES                   0
#define configMAX_CO_ROUTINE_PRIORITIES         2

/* Software timer related definitions. */
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               2
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            (configMINIMAL_STACK_SIZE * 2)

/*
Interrupt nesting behavior configuration.
PSoC 6 __NVIC_PRIO_BITS = 3

0 (high)
1           MAX_API_CALL_INTERRUPT_PRIORITY 001xxxxx (0x3F)
2
3
4
5
6
7 (low)     KERNEL_INTERRUPT_PRIORITY       111xxxxx (0xFF)
*/

/* Put KERNEL_INTERRUPT_PRIORITY in top __NVIC_PRIO_BITS bits of CM4 register */
#define configKERNEL_INTERRUPT_PRIORITY         0xFF

/* Put MAX_SYSCALL_INTERRUPT_PRIORITY in top __NVIC_PRIO_BITS bits of CM4 register */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    0x3F
#define configMAX_API_CALL_INTERRUPT_PRIORITY   configMAX_SYSCALL_INTERRUPT_PRIORITY

/* Set the following definitions to 1 to include the API function, or zero
to exclude the API function. */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_xResumeFromISR                  1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetIdleTaskHandle          0
#define INCLUDE_eTaskGetState                   0
#define INCLUDE_xEventGroupSetBitFromISR        1
#define INCLUDE_xTimerPendFunctionCall          1
#define INCLUDE_xTaskAbortDelay                 0
#define INCLUDE_xTaskGetHandle                  0
#define INCLUDE_xTaskResumeFromISR              1

/* Normal assert() semantics without relying on the provision of an assert.h
header file. */
#if defined(NDEBUG)
#define configASSERT(x) CY_UNUSED_PARAMETER(x)
#else
#define configASSERT(x) if ((x) == 0) { taskDISABLE_INTERRUPTS(); CY_HALT(); }
#endif

/* Definitions that map the FreeRTOS port interrupt handlers to their CMSIS
standard names. */
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

/* Dynamic Memory Allocation Schemes */
#define HEAP_ALLOCATION_TYPE1                   (1)     /* heap_1.c */
#define HEAP_ALLOCATION_TYPE2                   (2)     /* heap_2.c */
#define HEAP_ALLOCATION_TYPE3                   (3)     /* heap_3.c */
#define HEAP_ALLOCATION_TYPE4                   (4)     /* heap_4.c */
#define HEAP_ALLOCATION_TYPE5                   (5)     /* heap_5.c */
#define NO_HEAP_ALLOCATION                      (0)

#define configHEAP_ALLOCATION_SCHEME            (HEAP_ALLOCATION_TYPE3)

/* Check if the ModusToolbox Device Configurator Power personality parameter
 * "System Idle Power Mode" is set to either "CPU Sleep" or "System Deep Sleep".
 */
#if defined(CY_CFG_PWR_SYS_IDLE_MODE) && \
    ((CY_CFG_PWR_SYS_IDLE_MODE == CY_CFG_PWR_MODE_SLEEP) || \
     (CY_CFG_PWR_SYS_IDLE_MODE == CY_CFG_PWR_MODE_DEEPSLEEP))

extern void vApplicationSleep(uint32_t xExpectedIdleTime);
#define portSUPPRESS_TICKS_AND_SLEEP(xIdleTime) vApplicationSleep(xIdleTime)
#define configUSE_TICKLESS_IDLE                 2

#else
#define configUSE_TICKLESS_IDLE                 0
#endif

/* Deep Sleep Latency Configuration */
#if (CY_CFG_PWR_DEEPSLEEP_LATENCY > 0)
#define configEXPECTED_IDLE_TIME_BEFORE_SLEEP   CY_CFG_PWR_DEEPSLEEP_LATENCY
#endif

/* Allocate newlib reentrancy structures for each RTOS task. */
#define configUSE_NEWLIB_REENTRANT              1

#endif /* FREERTOS_CONFIG_H */
