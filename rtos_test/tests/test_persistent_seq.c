/*******************************************************************************
 * File Name:   test_persistent_seq.c
 *
 * Description: Host-side unit tests for persistent_seq (Em_EEPROM A/B banks).
 *
 * Scenarios:
 *   A — Fresh init (both banks erased)
 *   B — Recovery from bank A only
 *   C — Recovery from bank B only
 *   D — Both banks valid, A has higher seq
 *   E — Both banks valid, B has higher seq
 *   F — Corrupt bank A, recover from B
 *   G — Checkpoint writes to alternating banks
 *   H — Init → checkpoint → re-init round-trip
 *   I — Boot count increments across reboots
 *   J — Seq wrap-around (near UINT32_MAX)
 ******************************************************************************/

#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>

#include "persistent_seq.h"
#include "flash_mock.h"

/*******************************************************************************
 * Minimal test harness (same as other suites)
 ******************************************************************************/
static int tests_run = 0;
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

#define TEST_ASSERT_TRUE(cond) \
    TEST_ASSERT_MSG((cond), "expected true: %s", #cond)
#define TEST_ASSERT_EQ_U32(exp, act)                                          \
    TEST_ASSERT_MSG((uint32_t)(exp) == (uint32_t)(act),                       \
                    "expected %lu, got %lu",                                  \
                    (unsigned long)(uint32_t)(exp),                           \
                    (unsigned long)(uint32_t)(act))

/*******************************************************************************
 * Helper — write a valid bank directly via the mock
 ******************************************************************************/

/* Re-use the CRC routine — we duplicate a tiny table-less CRC here so the
 * test does not depend on internal persistent_seq symbols. */
static uint32_t test_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++)
        {
            if (crc & 1u)
                crc = (crc >> 1u) ^ 0xEDB88320UL;
            else
                crc >>= 1u;
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}

static void inject_bank(int bank, uint32_t seq, uint32_t boot_count)
{
    persistent_seq_bank_t buf;
    memset(&buf, 0xFF, sizeof(buf));
    buf.magic      = PERSISTENT_SEQ_MAGIC;
    buf.seq_value  = seq;
    buf.boot_count = boot_count;
    buf.crc32      = test_crc32(&buf, offsetof(persistent_seq_bank_t, crc32));
    flash_mock_bank_write(bank, &buf);
}

/*******************************************************************************
 * Test A — Fresh init (both banks 0xFF)
 ******************************************************************************/
static void test_A_fresh_init(void)
{
    printf("--- A: Fresh init ---\n");
    flash_mock_reset();

    TEST_ASSERT_TRUE(persistent_seq_init());
    TEST_ASSERT_EQ_U32(0u, persistent_seq_get());
    TEST_ASSERT_EQ_U32(1u, persistent_seq_get_boot_count());
}

/*******************************************************************************
 * Test B — Recovery from bank A only
 ******************************************************************************/
static void test_B_recover_bank_A(void)
{
    printf("--- B: Recover from bank A ---\n");
    flash_mock_reset();
    inject_bank(0, 500u, 3u);  /* Bank A valid */
    /* Bank B stays erased (0xFF) */

    TEST_ASSERT_TRUE(persistent_seq_init());
    TEST_ASSERT_EQ_U32(500u, persistent_seq_get());
    TEST_ASSERT_EQ_U32(4u,   persistent_seq_get_boot_count());
}

/*******************************************************************************
 * Test C — Recovery from bank B only
 ******************************************************************************/
static void test_C_recover_bank_B(void)
{
    printf("--- C: Recover from bank B ---\n");
    flash_mock_reset();
    /* Bank A stays erased */
    inject_bank(1, 1200u, 7u);

    TEST_ASSERT_TRUE(persistent_seq_init());
    TEST_ASSERT_EQ_U32(1200u, persistent_seq_get());
    TEST_ASSERT_EQ_U32(8u,    persistent_seq_get_boot_count());
}

/*******************************************************************************
 * Test D — Both valid, A has higher seq
 ******************************************************************************/
static void test_D_both_valid_A_higher(void)
{
    printf("--- D: Both valid, A higher ---\n");
    flash_mock_reset();
    inject_bank(0, 1000u, 10u);
    inject_bank(1,  900u,  9u);

    TEST_ASSERT_TRUE(persistent_seq_init());
    TEST_ASSERT_EQ_U32(1000u, persistent_seq_get());
    TEST_ASSERT_EQ_U32(11u,   persistent_seq_get_boot_count());
}

/*******************************************************************************
 * Test E — Both valid, B has higher seq
 ******************************************************************************/
static void test_E_both_valid_B_higher(void)
{
    printf("--- E: Both valid, B higher ---\n");
    flash_mock_reset();
    inject_bank(0,  800u, 5u);
    inject_bank(1, 1500u, 6u);

    TEST_ASSERT_TRUE(persistent_seq_init());
    TEST_ASSERT_EQ_U32(1500u, persistent_seq_get());
    TEST_ASSERT_EQ_U32(7u,    persistent_seq_get_boot_count());
}

/*******************************************************************************
 * Test F — Corrupt bank A, recover from B
 ******************************************************************************/
static void test_F_corrupt_A_recover_B(void)
{
    printf("--- F: Corrupt A, recover B ---\n");
    flash_mock_reset();
    inject_bank(0, 2000u, 20u);
    inject_bank(1, 1800u, 19u);
    flash_mock_corrupt_bank(0);  /* Corrupt A */

    TEST_ASSERT_TRUE(persistent_seq_init());
    TEST_ASSERT_EQ_U32(1800u, persistent_seq_get());
    TEST_ASSERT_EQ_U32(20u,   persistent_seq_get_boot_count());
}

/*******************************************************************************
 * Test G — Checkpoint writes to alternating banks
 ******************************************************************************/
static void test_G_checkpoint_pingpong(void)
{
    printf("--- G: Checkpoint ping-pong ---\n");
    flash_mock_reset();

    /* Fresh init writes to bank B (source=-1, write_bank=1) */
    persistent_seq_init();

    /* After init, next checkpoint goes to bank A (s_next_bank = 0) */
    persistent_seq_checkpoint(100u);
    TEST_ASSERT_EQ_U32(100u, persistent_seq_get());

    /* Read bank A to confirm it was written */
    const persistent_seq_bank_t *a = flash_mock_bank_read(0);
    TEST_ASSERT_EQ_U32(PERSISTENT_SEQ_MAGIC, a->magic);
    TEST_ASSERT_EQ_U32(100u, a->seq_value);

    /* Next checkpoint goes to bank B */
    persistent_seq_checkpoint(200u);
    const persistent_seq_bank_t *b = flash_mock_bank_read(1);
    TEST_ASSERT_EQ_U32(PERSISTENT_SEQ_MAGIC, b->magic);
    TEST_ASSERT_EQ_U32(200u, b->seq_value);

    /* Next back to A */
    persistent_seq_checkpoint(300u);
    a = flash_mock_bank_read(0);
    TEST_ASSERT_EQ_U32(300u, a->seq_value);
}

/*******************************************************************************
 * Test H — Init → checkpoint → re-init round-trip
 ******************************************************************************/
static void test_H_round_trip(void)
{
    printf("--- H: Round-trip ---\n");
    flash_mock_reset();

    /* Boot 1: fresh init */
    TEST_ASSERT_TRUE(persistent_seq_init());
    TEST_ASSERT_EQ_U32(0u, persistent_seq_get());
    TEST_ASSERT_EQ_U32(1u, persistent_seq_get_boot_count());

    /* Simulate runtime: checkpoint at seq=500 */
    persistent_seq_checkpoint(500u);
    TEST_ASSERT_EQ_U32(500u, persistent_seq_get());

    /* Boot 2: re-init should recover seq=500 */
    TEST_ASSERT_TRUE(persistent_seq_init());
    TEST_ASSERT_EQ_U32(500u, persistent_seq_get());
    TEST_ASSERT_EQ_U32(2u,   persistent_seq_get_boot_count());
}

/*******************************************************************************
 * Test I — Boot count increments across reboots
 ******************************************************************************/
static void test_I_boot_count_increments(void)
{
    printf("--- I: Boot count ---\n");
    flash_mock_reset();

    for (uint32_t expected_boot = 1u; expected_boot <= 5u; expected_boot++)
    {
        persistent_seq_init();
        TEST_ASSERT_EQ_U32(expected_boot, persistent_seq_get_boot_count());
        /* Checkpoint to persist the boot state (so next init sees it) */
        persistent_seq_checkpoint(expected_boot * 100u);
    }
}

/*******************************************************************************
 * Test J — Seq wrap-around near UINT32_MAX
 ******************************************************************************/
static void test_J_seq_wraparound(void)
{
    printf("--- J: Seq wrap-around ---\n");
    flash_mock_reset();

    /* Inject: A has seq near max, B has wrapped past zero */
    inject_bank(0, 0xFFFFFFF0UL, 50u);
    inject_bank(1, 0x00000010UL, 51u);

    TEST_ASSERT_TRUE(persistent_seq_init());
    /* B (0x10) is "later" than A (0xFFFFFFF0) when treated as signed diff.
     * diff = A - B = 0xFFFFFFF0 - 0x10 = 0xFFFFFFE0 → (int32_t) = -32
     * Since diff < 0, bank B wins. */
    TEST_ASSERT_EQ_U32(0x00000010UL, persistent_seq_get());
    TEST_ASSERT_EQ_U32(52u, persistent_seq_get_boot_count());
}

/*******************************************************************************
 * Test K — Equal seq, B has higher boot_count (tie-break)
 ******************************************************************************/
static void test_K_equal_seq_tiebreak(void)
{
    printf("--- K: Equal seq, B higher boot_count ---\n");
    flash_mock_reset();

    /* Both banks have seq=0, but B has a higher boot_count.
     * This happens after two boots with no checkpoint traffic. */
    inject_bank(0, 0u, 2u);
    inject_bank(1, 0u, 3u);

    TEST_ASSERT_TRUE(persistent_seq_init());
    TEST_ASSERT_EQ_U32(0u, persistent_seq_get());
    /* Should recover from B (boot_count=3), then bump to 4 */
    TEST_ASSERT_EQ_U32(4u, persistent_seq_get_boot_count());
}

/*******************************************************************************
 * Test L — Multi-boot with no checkpoint (boot_count must not stall)
 ******************************************************************************/
static void test_L_multi_boot_no_checkpoint(void)
{
    printf("--- L: Multi-boot no checkpoint ---\n");
    flash_mock_reset();

    /* Simulate 5 consecutive boots with NO checkpoint traffic.
     * boot_count must advance by 1 on every init(). */
    for (uint32_t expected_boot = 1u; expected_boot <= 5u; expected_boot++)
    {
        TEST_ASSERT_TRUE(persistent_seq_init());
        TEST_ASSERT_EQ_U32(0u, persistent_seq_get());
        TEST_ASSERT_EQ_U32(expected_boot, persistent_seq_get_boot_count());
        /* No checkpoint — seq stays 0 in both banks */
    }
}

/*******************************************************************************
 * Main
 ******************************************************************************/
int main(void)
{
    printf("=== test_persistent_seq ===\n\n");

    test_A_fresh_init();
    test_B_recover_bank_A();
    test_C_recover_bank_B();
    test_D_both_valid_A_higher();
    test_E_both_valid_B_higher();
    test_F_corrupt_A_recover_B();
    test_G_checkpoint_pingpong();
    test_H_round_trip();
    test_I_boot_count_increments();
    test_J_seq_wraparound();
    test_K_equal_seq_tiebreak();
    test_L_multi_boot_no_checkpoint();

    printf("\n=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0)
        printf(" (%d FAILED)", tests_failed);
    printf(" ===\n");

    return tests_failed > 0 ? 1 : 0;
}
