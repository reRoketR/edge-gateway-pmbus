#include <stdio.h>
#include <stdint.h>

#define GW_PROFILE_HEADER "profiles/profile_hil_smbalert.h"
#include "../source/gateway_config.c"

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

static void test_hil_profile_matches_runbook(void)
{
    printf("--- test_hil_profile_matches_runbook ---\n");

    TEST_ASSERT_TRUE(g_config.devices != NULL);
    TEST_ASSERT_TRUE(g_config.smbalert_enabled);
    TEST_ASSERT_EQ_U32(1u, g_config.num_devices);
    TEST_ASSERT_EQ_U32(0x58u, g_config.devices[0].addr_7bit);
    TEST_ASSERT_EQ_U32(5000u, g_config.devices[0].poll_period_ms);
    TEST_ASSERT_EQ_U32(5000u, g_config.devices[0].status_period_ms);
    TEST_ASSERT_EQ_U32(1000u, g_config.metrics_period_ms);
}

int main(void)
{
    printf("=== HIL SMBALERT Profile Test ===\n");

    test_hil_profile_matches_runbook();

    printf("\n=== Results: %d passed, %d failed, %d total ===\n",
           tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}
