#include <stdio.h>
#include <stdint.h>

#include "../source/gateway_config.h"

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

#define TEST_ASSERT_TRUE(cond)                                                \
    TEST_ASSERT_MSG((cond), "expected true: %s", #cond)

#define TEST_ASSERT_EQ_U32(exp, act)                                          \
    TEST_ASSERT_MSG((uint32_t)(exp) == (uint32_t)(act),                       \
                    "expected %lu, got %lu",                                  \
                    (unsigned long)(uint32_t)(exp),                           \
                    (unsigned long)(uint32_t)(act))

static void test_default_profile_matches_single_target_baseline(void)
{
    printf("--- test_default_profile_matches_single_target_baseline ---\n");

    TEST_ASSERT_TRUE(g_config.devices != NULL);
    TEST_ASSERT_EQ_U32(1u, g_config.num_devices);
    TEST_ASSERT_EQ_U32(0x58u, g_config.devices[0].addr_7bit);
    TEST_ASSERT_EQ_U32(61u, g_config.buffer.flash_max_records);
}

int main(void)
{
    printf("=== Default Profile Test ===\n");

    test_default_profile_matches_single_target_baseline();

    printf("\n=== Results: %d passed, %d failed, %d total ===\n",
           tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}
