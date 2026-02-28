/*******************************************************************************
 * File Name:   gw_util.h
 *
 * Description: Shared utility helpers for the gateway firmware.
 *
 ******************************************************************************/

#pragma once

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Format uint64_t as a decimal string.
 *
 * Portable implementation that avoids %llu issues on MinGW.
 *
 * @param[out] buf   Output buffer.
 * @param[in]  sz    Size of @p buf in bytes.
 * @param[in]  val   Value to format.
 * @return Number of characters written (excluding NUL), or negative on error.
 */
int fmt_u64(char *buf, size_t sz, uint64_t val);
