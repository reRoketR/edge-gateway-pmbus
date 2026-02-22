/*******************************************************************************
 * File Name:   pmbus_decode.c
 *
 * Description: PMBus data format decode/encode — pure functions, no HW deps.
 *
 *              Linear11:  Y × 2^N   (11-bit signed mantissa, 5-bit signed exp)
 *              Linear16:  Y × 2^N   (16-bit unsigned mantissa, separate exp)
 *
 * Related Document: PMBus spec part II §7.1, agent.md §3
 *
 ******************************************************************************/

#include "pmbus_decode.h"

/*******************************************************************************
 * Linear11 decode / encode
 ******************************************************************************/

/**
 * Extract signed 11-bit mantissa and signed 5-bit exponent from raw Linear11.
 */
static void linear11_unpack(uint16_t raw, int16_t *mantissa, int8_t *exponent)
{
    /* Exponent: bits [15:11], signed 5-bit → sign-extend to int8 */
    int8_t exp5 = (int8_t)(raw >> 11);
    if (exp5 > 15)
    {
        exp5 -= 32;  /* sign-extend 5-bit two's complement */
    }

    /* Mantissa: bits [10:0], signed 11-bit → sign-extend to int16 */
    int16_t man11 = (int16_t)(raw & 0x07FFu);
    if (man11 > 1023)
    {
        man11 -= 2048;  /* sign-extend 11-bit two's complement */
    }

    *mantissa = man11;
    *exponent = exp5;
}

int32_t pmbus_linear11_to_milli(uint16_t raw)
{
    int16_t mantissa;
    int8_t  exponent;
    linear11_unpack(raw, &mantissa, &exponent);

    /*
     * Result = mantissa × 2^exponent × 1000
     *
     * To maximize precision for negative exponents, multiply mantissa by 1000
     * first, then shift. For positive exponents, shift mantissa first to
     * avoid overflow, then multiply.
     */
    int32_t result;

    if (exponent >= 0)
    {
        result = ((int32_t)mantissa << exponent) * 1000;
    }
    else
    {
        /* Multiply by 1000 first, then divide by 2^|exp| with rounding */
        int32_t scaled = (int32_t)mantissa * 1000;
        int8_t  shift  = (int8_t)(-exponent);

        /* Round to nearest (add half the divisor before shifting) */
        if (scaled >= 0)
        {
            result = (scaled + (1 << (shift - 1))) >> shift;
        }
        else
        {
            result = -((-scaled + (1 << (shift - 1))) >> shift);
        }
    }

    return result;
}

float pmbus_linear11_to_float(uint16_t raw)
{
    int16_t mantissa;
    int8_t  exponent;
    linear11_unpack(raw, &mantissa, &exponent);

    float result = (float)mantissa;

    if (exponent >= 0)
    {
        for (int8_t i = 0; i < exponent; i++)
        {
            result *= 2.0f;
        }
    }
    else
    {
        for (int8_t i = 0; i < -exponent; i++)
        {
            result *= 0.5f;
        }
    }

    return result;
}

uint16_t pmbus_linear11_from_milli(int32_t milli_val)
{
    /*
     * Strategy: find the best exponent such that:
     *   mantissa = milli_val / 1000 / 2^exp
     *   mantissa fits in [-1024, +1023]
     *
     * We try exponents from -16 to +15 and pick the one with best precision.
     */
    if (milli_val == 0)
    {
        return 0u;
    }

    /* Convert milli to actual value × 2^16 for fixed-point precision */
    int32_t val_x1000 = milli_val;  /* Already in milli-units */

    int8_t  best_exp = 0;
    int16_t best_man = 0;
    int32_t best_err = 0x7FFFFFFF;

    for (int8_t exp = -16; exp <= 15; exp++)
    {
        int32_t man;
        if (exp >= 0)
        {
            /* mantissa = milli_val / (1000 * 2^exp) */
            int32_t divisor = 1000 * (1 << exp);
            man = (val_x1000 + divisor / 2) / divisor;
        }
        else
        {
            /* mantissa = milli_val * 2^|exp| / 1000 */
            int32_t shifted = val_x1000 * (1 << (-exp));
            man = (shifted + 500) / 1000;
        }

        /* Check mantissa range */
        if (man < -1024 || man > 1023)
        {
            continue;
        }

        /* Compute reconstruction error */
        int32_t reconstructed;
        if (exp >= 0)
        {
            reconstructed = man * (1 << exp) * 1000;
        }
        else
        {
            reconstructed = (man * 1000) >> (-exp);
        }

        int32_t err = milli_val - reconstructed;
        if (err < 0) err = -err;

        if (err < best_err)
        {
            best_err = err;
            best_man = (int16_t)man;
            best_exp = exp;
        }
    }

    /* Pack into Linear11: [exp(5)][man(11)] */
    uint16_t packed = 0u;
    packed |= (uint16_t)((uint8_t)(best_exp & 0x1Fu)) << 11u;
    packed |= (uint16_t)(best_man & 0x07FFu);

    return packed;
}

/*******************************************************************************
 * Linear16 decode (for READ_VOUT)
 ******************************************************************************/

uint32_t pmbus_linear16_to_mv(uint16_t raw, int8_t vout_exp)
{
    /*
     * Result = raw × 2^vout_exp × 1000 (in millivolts)
     *
     * Typical vout_exp is -12 to -9, so we multiply by 1000 first.
     */
    if (vout_exp >= 0)
    {
        return (uint32_t)raw * 1000u * (1u << (uint8_t)vout_exp);
    }
    else
    {
        uint8_t shift = (uint8_t)(-vout_exp);
        uint32_t scaled = (uint32_t)raw * 1000u;

        /* Round to nearest */
        return (scaled + (1u << (shift - 1u))) >> shift;
    }
}

float pmbus_linear16_to_float(uint16_t raw, int8_t vout_exp)
{
    float result = (float)raw;

    if (vout_exp >= 0)
    {
        for (int8_t i = 0; i < vout_exp; i++)
        {
            result *= 2.0f;
        }
    }
    else
    {
        for (int8_t i = 0; i < -vout_exp; i++)
        {
            result *= 0.5f;
        }
    }

    return result;
}

int8_t pmbus_vout_mode_exponent(uint8_t vout_mode)
{
    /* Bits [7:5] = mode.  Mode 0b000 = Linear. Others are not supported. */
    uint8_t mode = (vout_mode >> 5u) & 0x07u;
    if (mode != 0u)
    {
        return 0;  /* Not linear mode — return 0 as safe default */
    }

    /* Bits [4:0] = signed 5-bit exponent */
    int8_t exp5 = (int8_t)(vout_mode & 0x1Fu);
    if (exp5 > 15)
    {
        exp5 -= 32;  /* Sign-extend */
    }

    return exp5;
}
