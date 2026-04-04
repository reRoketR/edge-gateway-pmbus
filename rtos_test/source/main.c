/*******************************************************************************
* File Name:   main.c
*
* Description: PMBus-MQTT Edge Gateway — entry point.
*              Initializes BSP, retarget-io (UART debug), logs the active
*              configuration profile, and starts the FreeRTOS scheduler.
*
* Related Document: See agent.md, README.md
*
*******************************************************************************
* Copyright 2021-2024, Cypress Semiconductor Corporation (an Infineon company)
* SPDX-License-Identifier: Apache-2.0
*******************************************************************************/

/*******************************************************************************
* Header Files
*******************************************************************************/
#include "cyhal.h"
#include "cybsp.h"
#include "cy_retarget_io.h"

#include <stdio.h>

/* FreeRTOS */
#include "FreeRTOS.h"
#include "task.h"

/* Configuration */
#include "gateway_config.h"
#include "mqtt_client_config.h"
#include "wifi_config.h"

/* Gateway modules */
#include "gateway_ipc.h"
#include "metrics.h"
#include "buffer_mgr.h"
#include "pmbus_poll_task.h"
#include "mqtt_gw_task.h"
#include "emergency_ring.h"
#include "qspi_flash.h"
#include "persistent_seq.h"

/******************************************************************************
* Defines
*******************************************************************************/

/******************************************************************************
* Macros
*******************************************************************************/
#define BLINKY_TASK_STACK_SIZE  (256u)
#define BLINKY_TASK_PRIORITY   (1u)

/*******************************************************************************
* Function Prototypes
*******************************************************************************/
static void blinky_task(void *pvParameters);
void vApplicationDaemonTaskStartupHook(void);

/*******************************************************************************
* Function Name: main
*********************************************************************************
* Summary:
*  System entrance point. Initializes BSP, retarget-io for UART debug
*  logging, prints configuration profile information, creates initial
*  tasks, and starts the FreeRTOS scheduler.
*
* Parameters:
*  void
*
* Return:
*  int — never returns
*******************************************************************************/
int main(void)
{
    cy_rslt_t result;

#if defined(CY_DEVICE_SECURE)
    cyhal_wdt_t wdt_obj;
    /* Clear watchdog timer so that it doesn't trigger a reset */
    result = cyhal_wdt_init(&wdt_obj, cyhal_wdt_get_max_timeout_ms());
    CY_ASSERT(CY_RSLT_SUCCESS == result);
    cyhal_wdt_free(&wdt_obj);
#endif

    /* Initialize the board support package. */
    result = cybsp_init();
    CY_ASSERT(CY_RSLT_SUCCESS == result);

    /* Enable global interrupts. */
    __enable_irq();

    /* Initialize retarget-io to use the debug UART port. */
    result = cy_retarget_io_init(CYBSP_DEBUG_UART_TX, CYBSP_DEBUG_UART_RX,
                                 CY_RETARGET_IO_BAUDRATE);
    CY_ASSERT(CY_RSLT_SUCCESS == result);

    /* Clear screen and print banner */
    printf("\x1b[2J\x1b[;H");
    printf("============================================================\n");
    printf("  PMBus-MQTT Edge Gateway  |  CY8CKIT-062S2-43012\n");
    printf("============================================================\n");
    printf("  Build: " __DATE__ " " __TIME__ "\n");
    printf("============================================================\n");

    /* Log active config profile (thesis reproducibility — agent.md §5.4) */
    config_print_boot_banner();
    printf("[SYS] Wi-Fi SSID=%s\n\n", WIFI_SSID);

    /* ---- Initialise gateway subsystems ---- */

    /* Metrics module — zeroes all counters and ring buffers */
    metrics_init();
    printf("[SYS] Metrics initialised\n");

    /* IPC queues (telemetry, status, event) + seq counter */
    if (!gateway_ipc_init())
    {
        printf("[SYS] FATAL: IPC init failed\n");
        CY_ASSERT(0);  /* breakpoint in Debug */
        for (;;) { __WFI(); }  /* halt in Release */
    }

    /* Post system-boot event with boot_count in detail field */
    {
        char detail[EVT_DETAIL_MAX];
        snprintf(detail, sizeof(detail), "boot_count=%lu",
                 (unsigned long)persistent_seq_get_boot_count());
        gateway_ipc_post_event(EVT_SYSTEM_BOOT, detail);
    }

    /* Expose boot_count in metrics gauges */
    metrics_set_boot_count(persistent_seq_get_boot_count());

    emergency_ring_init();

    /* QSPI external flash (S25FL512S) — D2a-1
     * NOTE: Only init here (no mutex needed for HAL + get_size).
     * Self-test uses erase/write which acquire a FreeRTOS mutex inside
     * the serial-flash library, so it must run AFTER the scheduler starts.
     */
    result = qspi_flash_init();
    if (result != CY_RSLT_SUCCESS)
    {
        printf("[QSPI] WARNING: QSPI flash init failed, continuing without external flash\n");
    }

    /* Store-and-forward buffer (RAM ring buffer + optional persistent tier).
     * QSPI backend recovery reads the memory-mapped external flash, so SMIF
     * must already be initialized before buffer_mgr_init() runs.
     */
    if (!buffer_mgr_init())
    {
        printf("[SYS] FATAL: Buffer manager init failed\n");
        CY_ASSERT(0);
        for (;;) { __WFI(); }
    }

    /* ---- Create gateway tasks (agent.md §6) ---- */

    /* Task A — PMBus polling (high priority) */
    xTaskCreate(pmbus_poll_task, "PMBus_Poll",
                PMBUS_POLL_TASK_STACK_SIZE, NULL,
                PMBUS_POLL_TASK_PRIORITY, NULL);

#if !defined(GW_DISABLE_MQTT)
    /* Task B — MQTT gateway (medium priority) */
    xTaskCreate(mqtt_gw_task, "MQTT_GW",
                MQTT_GW_TASK_STACK_SIZE, NULL,
                MQTT_GW_TASK_PRIORITY, NULL);

    /* Task C — Buffer flush (low-medium priority) */
    xTaskCreate(buffer_task, "Buffer",
                BUFFER_TASK_STACK_SIZE, NULL,
                BUFFER_TASK_PRIORITY, NULL);
#else
    printf("[SYS] GW_DISABLE_MQTT: MQTT and Buffer tasks suppressed\n");
#endif /* GW_DISABLE_MQTT */

    /* Blinky — heartbeat LED (lowest priority) */
    xTaskCreate(blinky_task, "Blinky", BLINKY_TASK_STACK_SIZE,
                NULL, BLINKY_TASK_PRIORITY, NULL);

    printf("[SYS] All tasks created — starting scheduler\n\n");

    /* Start the FreeRTOS scheduler. */
    vTaskStartScheduler();

    /* Should never get here. */
    CY_ASSERT(0);
}

/*******************************************************************************
* Function Name: blinky_task
*********************************************************************************
* Summary:
*  Temporary task that blinks the user LED to confirm FreeRTOS is running.
*  Will be replaced by gateway tasks (pmbus_poll, mqtt, buffer) in later steps.
*
* Parameters:
*  pvParameters — unused
*******************************************************************************/
static void blinky_task(void *pvParameters)
{
    (void)pvParameters;

    cyhal_gpio_init(CYBSP_USER_LED, CYHAL_GPIO_DIR_OUTPUT,
                    CYHAL_GPIO_DRIVE_STRONG, CYBSP_LED_STATE_OFF);

    for (;;)
    {
        cyhal_gpio_toggle(CYBSP_USER_LED);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/*******************************************************************************
* Function Name: vApplicationDaemonTaskStartupHook
*********************************************************************************
* Summary:
*  Called once after the scheduler starts. Can be used for late initialization.
*******************************************************************************/
void vApplicationDaemonTaskStartupHook(void)
{
    /* QSPI self-test — must run inside a task context because the
     * serial-flash library uses a FreeRTOS mutex for thread safety.
     * erase/write/read operations would deadlock if called from main()
     * before vTaskStartScheduler().
     */
    if (qspi_flash_get_size() > 0u)
    {
        qspi_flash_self_test();
    }

    /* Persistent backend recovery/format may also need erase/write, so finish
     * that step only after the scheduler is running.
     */
    buffer_mgr_late_init();
}

/* [] END OF FILE */
