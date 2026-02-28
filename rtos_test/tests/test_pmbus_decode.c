/*******************************************************************************
 * File Name:   test_pmbus_decode.c
 *
 * Description: Host-side unit tests for pmbus_decode (Linear11/Linear16)
 *              and pmbus_crc8 (PEC).
 *
 *              Compile and run on PC (no MCU needed):
 *                gcc -o test_pmbus_decode test_pmbus_decode.c
 *                    ../source/pmbus_decode.c -lm
 *                ./test_pmbus_decode
 *
 *              Or on Windows with MSVC:
 *                cl test_pmbus_decode.c ../source/pmbus_decode.c
 *                test_pmbus_decode.exe
 *
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

/* Module under test */
#include "../source/pmbus_decode.h"

/*******************************************************************************
 * Minimal test framework
 ******************************************************************************/
static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT_MSG(cond, fmt, ...)                                      \
    do {                                                                      \
        tests_run++;                                                          \
        if (cond) {                                                           \
            tests_passed++;                                                   \
        } else {                                                              \
            tests_failed++;                                                   \
            printf("  FAIL [%s:%d]: " fmt "\n", __FILE__, __LINE__,           \
                   ##__VA_ARGS__);                                            \
        }                                                                     \
    } while (0)

#define TEST_ASSERT_INT_EQ(expected, actual)                                  \
    TEST_ASSERT_MSG((expected) == (actual),                                    \
                    "expected %d, got %d", (int)(expected), (int)(actual))

#define TEST_ASSERT_UINT_EQ(expected, actual)                                 \
    TEST_ASSERT_MSG((expected) == (actual),                                    \
                    "expected %u, got %u",                                     \
                    (unsigned)(expected), (unsigned)(actual))

#define TEST_ASSERT_FLOAT_NEAR(expected, actual, tol)                         \
    TEST_ASSERT_MSG(fabsf((expected) - (actual)) <= (tol),                    \
                    "expected %.6f, got %.6f (tol %.6f)",                      \
                    (double)(expected), (double)(actual), (double)(tol))

/*******************************************************************************
 * Standalone PEC (CRC-8) implementation for testing
 * (same algorithm as pmbus_master.c, duplicated here to avoid HW includes)
 ******************************************************************************/
static uint8_t test_crc8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0x00u;
    for (uint8_t i = 0u; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t bit = 0u; bit < 8u; bit++)
        {
            if (crc & 0x80u)
                crc = (uint8_t)((crc << 1u) ^ 0x07u);
            else
                crc <<= 1u;
        }
    }
    return crc;
}

/*******************************************************************************
 * Helper: manually encode Linear11
 ******************************************************************************/
static uint16_t make_linear11(int8_t exponent, int16_t mantissa)
{
    uint16_t raw = 0u;
    raw |= (uint16_t)((uint8_t)(exponent & 0x1Fu)) << 11u;
    raw |= (uint16_t)(mantissa & 0x07FFu);
    return raw;
}

/*******************************************************************************
 * Test: Linear11 decode known values
 ******************************************************************************/
static void test_linear11_known_values(void)
{
    printf("--- test_linear11_known_values ---\n");

    /* 3.3V: mantissa=825, exponent=-8 → 825/256 = 3.22265..
     * Better: mantissa=825, exp=-8 → 3.222
     * Or:     mantissa=422, exp=-7 → 422/128 = 3.296875
     * Or:     mantissa=211, exp=-6 → 211/64  = 3.296875
     * Exact:  mantissa=844, exp=-8 → 844/256 = 3.296875 (close to 3.3)
     *
     * Let's use a simpler known value: 2.0V = mantissa=2, exp=0
     */

    /* Value = 2.0: mantissa=2, exponent=0 */
    {
        uint16_t raw = make_linear11(0, 2);
        int32_t milli = pmbus_linear11_to_milli(raw);
        float   fval  = pmbus_linear11_to_float(raw);
        TEST_ASSERT_INT_EQ(2000, milli);
        TEST_ASSERT_FLOAT_NEAR(2.0f, fval, 0.001f);
    }

    /* Value = 1.0: mantissa=1, exponent=0 */
    {
        uint16_t raw = make_linear11(0, 1);
        TEST_ASSERT_INT_EQ(1000, pmbus_linear11_to_milli(raw));
        TEST_ASSERT_FLOAT_NEAR(1.0f, pmbus_linear11_to_float(raw), 0.001f);
    }

    /* Value = 0.5: mantissa=1, exponent=-1 */
    {
        uint16_t raw = make_linear11(-1, 1);
        TEST_ASSERT_INT_EQ(500, pmbus_linear11_to_milli(raw));
        TEST_ASSERT_FLOAT_NEAR(0.5f, pmbus_linear11_to_float(raw), 0.001f);
    }

    /* Value = -0.5: mantissa=-1, exponent=-1 */
    {
        uint16_t raw = make_linear11(-1, -1);
        TEST_ASSERT_INT_EQ(-500, pmbus_linear11_to_milli(raw));
        TEST_ASSERT_FLOAT_NEAR(-0.5f, pmbus_linear11_to_float(raw), 0.001f);
    }

    /* Value = 12.5: mantissa=100, exponent=-3 → 100/8 = 12.5 */
    {
        uint16_t raw = make_linear11(-3, 100);
        TEST_ASSERT_INT_EQ(12500, pmbus_linear11_to_milli(raw));
        TEST_ASSERT_FLOAT_NEAR(12.5f, pmbus_linear11_to_float(raw), 0.001f);
    }

    /* Value = 256: mantissa=1, exponent=8 → 1*256=256 */
    {
        uint16_t raw = make_linear11(8, 1);
        TEST_ASSERT_INT_EQ(256000, pmbus_linear11_to_milli(raw));
        TEST_ASSERT_FLOAT_NEAR(256.0f, pmbus_linear11_to_float(raw), 0.1f);
    }

    /* Value = 0 */
    {
        uint16_t raw = make_linear11(0, 0);
        TEST_ASSERT_INT_EQ(0, pmbus_linear11_to_milli(raw));
        TEST_ASSERT_FLOAT_NEAR(0.0f, pmbus_linear11_to_float(raw), 0.001f);
    }

    /* Negative exponent, larger mantissa:
     * Value ≈ 3.296875: mantissa=422, exponent=-7 → 422/128 = 3.296875 */
    {
        uint16_t raw = make_linear11(-7, 422);
        int32_t milli = pmbus_linear11_to_milli(raw);
        float   fval  = pmbus_linear11_to_float(raw);
        /* 422/128 * 1000 = 3296.875 → rounded to 3297 */
        TEST_ASSERT_MSG(abs(milli - 3297) <= 1,
                        "expected ~3297, got %d", (int)milli);
        TEST_ASSERT_FLOAT_NEAR(3.296875f, fval, 0.001f);
    }
}

/*******************************************************************************
 * Test: Linear11 round-trip (encode → decode)
 ******************************************************************************/
static void test_linear11_roundtrip(void)
{
    printf("--- test_linear11_roundtrip ---\n");

    int32_t test_values[] = {
        0, 1000, -1000, 500, 3300, 12000, 48000, -250, 100, 50
    };

    for (size_t i = 0; i < sizeof(test_values)/sizeof(test_values[0]); i++)
    {
        int32_t original = test_values[i];
        uint16_t encoded = pmbus_linear11_from_milli(original);
        int32_t decoded  = pmbus_linear11_to_milli(encoded);

        /* Allow ±2 milli-unit error due to quantization */
        int32_t error = decoded - original;
        if (error < 0) error = -error;

        TEST_ASSERT_MSG(error <= 2,
                        "roundtrip %d → 0x%04X → %d (error=%d)",
                        (int)original, (unsigned)encoded,
                        (int)decoded, (int)error);
    }
}

/*******************************************************************************
 * Test: Linear16 decode known values
 ******************************************************************************/
static void test_linear16_known_values(void)
{
    printf("--- test_linear16_known_values ---\n");

    /* Typical: VOUT_MODE exp = -12, raw for 3.3V:
     * 3.3 = raw * 2^-12 → raw = 3.3 * 4096 = 13517
     * Decode: 13517 * 1000 / 4096 = 3300.04... → 3300
     */
    {
        uint32_t mv = pmbus_linear16_to_mv(13517, -12);
        TEST_ASSERT_MSG(abs((int)mv - 3300) <= 1,
                        "expected ~3300, got %u", (unsigned)mv);

        float fval = pmbus_linear16_to_float(13517, -12);
        TEST_ASSERT_FLOAT_NEAR(3.3f, fval, 0.001f);
    }

    /* 1.0V with exp=-10: raw = 1.0 * 1024 = 1024 */
    {
        uint32_t mv = pmbus_linear16_to_mv(1024, -10);
        TEST_ASSERT_UINT_EQ(1000, mv);

        float fval = pmbus_linear16_to_float(1024, -10);
        TEST_ASSERT_FLOAT_NEAR(1.0f, fval, 0.001f);
    }

    /* 12.0V with exp=-9: raw = 12.0 * 512 = 6144 */
    {
        uint32_t mv = pmbus_linear16_to_mv(6144, -9);
        TEST_ASSERT_UINT_EQ(12000, mv);
    }

    /* Positive exponent (unusual but valid): exp=0, raw=5 → 5V */
    {
        uint32_t mv = pmbus_linear16_to_mv(5, 0);
        TEST_ASSERT_UINT_EQ(5000, mv);
    }
}

/*******************************************************************************
 * Test: VOUT_MODE exponent extraction
 ******************************************************************************/
static void test_vout_mode_exponent(void)
{
    printf("--- test_vout_mode_exponent ---\n");

    /* Mode = 0b000 (Linear), exponent = -12 (0b10100 = 20 unsigned → -12 signed) */
    /* -12 in 5-bit two's complement: 32 - 12 = 20 = 0x14 */
    {
        uint8_t vout_mode = 0x14u;  /* mode=000, exp=10100 = -12 */
        int8_t exp = pmbus_vout_mode_exponent(vout_mode);
        TEST_ASSERT_INT_EQ(-12, exp);
    }

    /* exp = -10 → 32-10 = 22 = 0x16 */
    {
        int8_t exp = pmbus_vout_mode_exponent(0x16u);
        TEST_ASSERT_INT_EQ(-10, exp);
    }

    /* exp = -9 → 32-9 = 23 = 0x17 */
    {
        int8_t exp = pmbus_vout_mode_exponent(0x17u);
        TEST_ASSERT_INT_EQ(-9, exp);
    }

    /* exp = 0 → 0x00 */
    {
        int8_t exp = pmbus_vout_mode_exponent(0x00u);
        TEST_ASSERT_INT_EQ(0, exp);
    }

    /* exp = 5 → 0x05 */
    {
        int8_t exp = pmbus_vout_mode_exponent(0x05u);
        TEST_ASSERT_INT_EQ(5, exp);
    }

    /* Non-linear mode (mode=001, bits[7:5]=0x20) → should return 0 */
    {
        int8_t exp = pmbus_vout_mode_exponent(0x20u);
        TEST_ASSERT_INT_EQ(0, exp);
    }
}

/*******************************************************************************
 * Test: PEC CRC-8
 ******************************************************************************/
static void test_pec_crc8(void)
{
    printf("--- test_pec_crc8 ---\n");

    /*
     * Known SMBus PEC test vector:
     * Data: { 0xA0, 0x01, 0xA1, 0x34, 0x56 }
     *   addr_w=0xA0 (target 0x50 + W)
     *   cmd=0x01
     *   addr_r=0xA1 (target 0x50 + R)
     *   data_low=0x34
     *   data_high=0x56
     *
     * We'll compute CRC and verify it's deterministic.
     */

    /* Test 1: Empty data → CRC = 0 */
    {
        uint8_t crc = test_crc8(NULL, 0);
        TEST_ASSERT_UINT_EQ(0u, crc);
    }

    /* Test 2: Single byte */
    {
        uint8_t data[] = { 0x01 };
        uint8_t crc = test_crc8(data, 1);
        /* CRC-8 of {0x01} with poly 0x07: manually computed = 0x07 */
        TEST_ASSERT_UINT_EQ(0x07u, crc);
    }

    /* Test 3: Verify PEC of known SMBus transaction.
     * According to SMBus spec example: addr=0x5B (7-bit), cmd=0x01
     * Write: [0xB6][0x01] → PEC
     * Read:  [0xB6][0x01][0xB7][data_l][data_h] → PEC
     */
    {
        /* Self-consistency: encode → append PEC → verify CRC over all = 0 */
        uint8_t msg[] = { 0xB6, 0x01, 0xB7, 0x34, 0x56 };
        uint8_t pec = test_crc8(msg, 5);

        /* Now verify that CRC over msg+pec = 0 (property of CRC) */
        uint8_t msg_with_pec[6];
        memcpy(msg_with_pec, msg, 5);
        msg_with_pec[5] = pec;
        uint8_t verify = test_crc8(msg_with_pec, 6);
        TEST_ASSERT_UINT_EQ(0u, verify);
    }

    /* Test 4: Known value — CRC-8/SMBUS of "123456789" = 0xF4 */
    {
        uint8_t data[] = { '1','2','3','4','5','6','7','8','9' };
        uint8_t crc = test_crc8(data, 9);
        TEST_ASSERT_UINT_EQ(0xF4u, crc);
    }
}

/*******************************************************************************
 * Test: Edge cases
 ******************************************************************************/
static void test_edge_cases(void)
{
    printf("--- test_edge_cases ---\n");

    /* Max positive mantissa (1023) with exp=0 → 1023.0 → 1023000 milli */
    {
        uint16_t raw = make_linear11(0, 1023);
        TEST_ASSERT_INT_EQ(1023000, pmbus_linear11_to_milli(raw));
    }

    /* Max negative mantissa (-1024) with exp=0 → -1024.0 → -1024000 milli */
    {
        uint16_t raw = make_linear11(0, -1024);
        TEST_ASSERT_INT_EQ(-1024000, pmbus_linear11_to_milli(raw));
    }

    /* Max positive exponent (15) with small mantissa → 1*32768 = 32768000 milli */
    {
        uint16_t raw = make_linear11(15, 1);
        TEST_ASSERT_INT_EQ(32768000, pmbus_linear11_to_milli(raw));
    }

    /* Min exponent (-16) with mantissa=1 → 1/65536 ≈ 0.015 → ~0 milli */
    {
        uint16_t raw = make_linear11(-16, 1);
        int32_t milli = pmbus_linear11_to_milli(raw);
        /* 1000/65536 = 0.0152... rounds to 0 */
        TEST_ASSERT_MSG(abs(milli) <= 1,
                        "expected ~0, got %d", (int)milli);
    }

    /* Linear16 raw=0 → 0 mV */
    {
        TEST_ASSERT_UINT_EQ(0u, pmbus_linear16_to_mv(0, -12));
    }

    /* Linear16 raw=0xFFFF with exp=-12 → 65535*1000/4096 = 15999 mV */
    {
        uint32_t mv = pmbus_linear16_to_mv(0xFFFF, -12);
        TEST_ASSERT_MSG(abs((int)mv - 15999) <= 1,
                        "expected ~15999, got %u", (unsigned)mv);
    }
}

/*******************************************************************************
 * Main
 ******************************************************************************/
int main(void)
{
    printf("=== PMBus Decode & PEC Unit Tests ===\n\n");

    test_linear11_known_values();
    test_linear11_roundtrip();
    test_linear16_known_values();
    test_vout_mode_exponent();
    test_pec_crc8();
    test_edge_cases();

    printf("\n=== Results: %d passed, %d failed, %d total ===\n",
           tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}
