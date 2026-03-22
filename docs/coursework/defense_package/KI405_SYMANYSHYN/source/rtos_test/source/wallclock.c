/**
 * @file wallclock.c
 * @brief Wall-clock (real-time) timestamps via lwIP SNTP.
 *
 * @details
 * Maintains a signed offset between the FreeRTOS monotonic tick and Unix
 * epoch.  The offset is set (and periodically refreshed) by the lwIP SNTP
 * client through the SNTP_SET_SYSTEM_TIME_US macro defined in lwipopts.h.
 *
 * Thread-safety: the offset is written only from the lwIP/tcpip thread
 * (SNTP callback context) and read from any task.  A critical section
 * protects the 64-bit read/write on the 32-bit CM4.
 */

#include "wallclock.h"

#include "FreeRTOS.h"
#include "task.h"

#include "lwip/apps/sntp.h"

#include <stdio.h>

/*******************************************************************************
 * Private state
 ******************************************************************************/

/** Offset: epoch_ms = tick_ms + s_offset_ms.  Protected by critical section. */
static volatile int64_t  s_offset_ms;

/** Set to true after the first successful SNTP response. */
static volatile bool     s_synced;

/** Guard against multiple sntp_init() calls. */
static bool              s_initialised;

/*******************************************************************************
 * SNTP callback — invoked by lwIP from the tcpip thread.
 *
 * lwipopts.h defines:
 *   #define SNTP_SET_SYSTEM_TIME_US(sec, us)  wallclock_sntp_set_time(sec, us)
 *
 * lwIP passes Unix-epoch seconds and microseconds.
 ******************************************************************************/
void wallclock_sntp_set_time(uint32_t sec, uint32_t us)
{
    /* Current FreeRTOS tick in ms */
    uint64_t tick_ms = (uint64_t)xTaskGetTickCount() * (uint64_t)portTICK_PERIOD_MS;

    /* Unix epoch ms from NTP */
    uint64_t epoch_ms = (uint64_t)sec * 1000u + (uint64_t)(us / 1000u);

    int64_t offset = (int64_t)epoch_ms - (int64_t)tick_ms;

    taskENTER_CRITICAL();
    s_offset_ms = offset;
    s_synced    = true;
    taskEXIT_CRITICAL();

    /* Human-readable log (first sync and every ~hour refresh) */
    uint32_t epoch_sec = sec;
    uint32_t hh  = (epoch_sec / 3600u) % 24u;
    uint32_t mm  = (epoch_sec / 60u)   % 60u;
    uint32_t ss  = epoch_sec % 60u;
    printf("[WALLCLOCK] SNTP synced — UTC %02lu:%02lu:%02lu  (offset %+lld ms)\n",
           (unsigned long)hh, (unsigned long)mm, (unsigned long)ss,
           (long long)offset);
}

/*******************************************************************************
 * Public API
 ******************************************************************************/

void wallclock_sntp_init(void)
{
    if (s_initialised)
    {
        return;
    }
    s_initialised = true;

    printf("[WALLCLOCK] Starting SNTP client...\n");

    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_setservername(1, "time.google.com");
    sntp_init();
}

uint64_t wallclock_now_ms(void)
{
    uint64_t tick_ms = (uint64_t)xTaskGetTickCount() * (uint64_t)portTICK_PERIOD_MS;

    if (!s_synced)
    {
        return tick_ms;   /* fallback: uptime ms */
    }

    int64_t offset;
    taskENTER_CRITICAL();
    offset = s_offset_ms;
    taskEXIT_CRITICAL();

    return (uint64_t)((int64_t)tick_ms + offset);
}

bool wallclock_is_synced(void)
{
    return s_synced;
}
