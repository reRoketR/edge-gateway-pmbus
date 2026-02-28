/**
 * @file wallclock.h
 * @brief Wall-clock (real-time) timestamps via SNTP synchronisation.
 *
 * @details
 * After Wi-Fi is up, call wallclock_sntp_init() once.  The lwIP SNTP client
 * will obtain UTC time from an NTP server and compute an offset between the
 * FreeRTOS tick counter and Unix epoch.  Subsequent calls to
 * wallclock_now_ms() return Unix-epoch milliseconds.
 *
 * If SNTP has not yet synchronised (e.g. NTP server unreachable), the
 * function falls back to returning milliseconds-since-boot (same as before).
 *
 * The module is safe to call from any FreeRTOS task.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Start the lwIP SNTP client (call once after Wi-Fi is connected).
 *
 * Configures poll mode with two DNS-resolved NTP servers and starts the
 * background SNTP process inside the lwIP/tcpip thread.
 *
 * Safe to call multiple times — subsequent calls are ignored.
 */
void wallclock_sntp_init(void);

/**
 * @brief Get current wall-clock time in milliseconds.
 *
 * @return  If SNTP-synced: Unix epoch milliseconds (UTC).
 *          If not yet synced: milliseconds since FreeRTOS scheduler start.
 */
uint64_t wallclock_now_ms(void);

/**
 * @brief Check whether the clock has been synchronised via SNTP.
 *
 * @return true if at least one successful SNTP response has been received.
 */
bool wallclock_is_synced(void);
