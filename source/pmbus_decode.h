/**
 * @file pmbus_decode.h
 * @brief PMBus data format decode/encode utilities.
 * @ingroup pmbus_decode
 *
 * @details
 * PMBus uses two fixed-point formats:
 *
 * 1. **Linear11** (16-bit) — for power, current, voltage (some):
 *    - Bits [15:11] = signed 5-bit exponent N (−16..+15)
 *    - Bits [10:0]  = signed 11-bit mantissa Y (−1024..+1023)
 *    - Real value = Y × 2^N
 *
 * 2. **Linear16** (16-bit) — for output voltage (READ_VOUT):
 *    - All 16 bits = unsigned mantissa Y
 *    - Exponent N is set separately via VOUT_MODE command
 *    - Real value = Y × 2^N
 *
 * All functions are pure (no side effects) and suitable for unit testing
 * on host.
 *
 * @see PMBus spec part II §7.1, agent.md §3
 *
 * @defgroup pmbus_decode PMBus Decode
 * @brief Linear11/Linear16 format decoders and VOUT_MODE exponent extraction.
 * @{
 */

#pragma once

#include <stdint.h>

/*******************************************************************************
 * Linear11 (5-bit exponent + 11-bit mantissa)
 ******************************************************************************/

/**
 * @brief Decode a PMBus Linear11 value to millivolts / milliamps / milliwatts.
 *
 * Converts the 16-bit raw PMBus reading to a signed 32-bit integer in
 * "milli-units" (×1000) to avoid floating point.
 *
 * Example: 3.3 V → 3300  ;  1.25 A → 1250  ;  -0.5 V → -500
 *
 * @param[in]  raw   Raw 16-bit Linear11 value from PMBus register
 * @return     Decoded value in milli-units (signed).
 */
int32_t pmbus_linear11_to_milli(uint16_t raw);

/**
 * @brief Decode a PMBus Linear11 value to a float.
 *
 * @param[in]  raw   Raw 16-bit Linear11 value
 * @return     Decoded real value.
 */
float pmbus_linear11_to_float(uint16_t raw);

/**
 * @brief Encode a milli-units value into PMBus Linear11.
 *
 * The encoder picks the best exponent to preserve precision.
 *
 * @param[in]  milli_val  Value in milli-units (e.g. 3300 for 3.3)
 * @return     Encoded 16-bit Linear11 value.
 */
uint16_t pmbus_linear11_from_milli(int32_t milli_val);

/*******************************************************************************
 * Linear16 (unsigned 16-bit mantissa, separate exponent)
 ******************************************************************************/

/**
 * @brief Decode a PMBus Linear16 VOUT value to millivolts.
 *
 * @param[in]  raw        Raw 16-bit unsigned mantissa from READ_VOUT
 * @param[in]  vout_exp   Signed exponent from VOUT_MODE (typically -12..-9)
 * @return     Decoded voltage in millivolts (unsigned).
 */
uint32_t pmbus_linear16_to_mv(uint16_t raw, int8_t vout_exp);

/**
 * @brief Decode a PMBus Linear16 VOUT value to float.
 *
 * @param[in]  raw        Raw 16-bit unsigned mantissa
 * @param[in]  vout_exp   Signed exponent from VOUT_MODE
 * @return     Decoded real voltage.
 */
float pmbus_linear16_to_float(uint16_t raw, int8_t vout_exp);

/**
 * @brief Extract the VOUT exponent from the VOUT_MODE register value.
 *
 * VOUT_MODE register format:
 *   Bits [7:5] = mode (must be 0b000 for Linear16)
 *   Bits [4:0] = signed 5-bit exponent
 *
 * @param[in]  vout_mode  Raw 8-bit VOUT_MODE register value
 * @return     Signed exponent (or 0 if mode is not linear).
 */
int8_t pmbus_vout_mode_exponent(uint8_t vout_mode);

/** @} */  /* end of pmbus_decode */
