/*******************************************************************************
 * File Name:   test_i2c_recovery.c
 *
 * Description: Host-side unit tests for the formalized PMBus I2C recovery
 *              split in pmbus_master.c.
 *
 * Verifies:
 *   - routing of TIMEOUT / NOT_READY -> controller reset
 *   - routing of BUS_FAULT -> SCL-based bus recovery
 *   - distinct events and success metrics for each path
 *   - recovery_settle_ms delay after successful recovery
 *
 * Build:
 *   make test
 *
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../source/gateway_config.h"
#include "../source/pmbus_master.h"
#include "../source/events.h"
#include "i2c_mock.h"

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

#define TEST_ASSERT_FALSE(cond)                                               \
    TEST_ASSERT_MSG(!(cond), "expected false: %s", #cond)

#define TEST_ASSERT_EQ_U32(expected, actual)                                  \
    TEST_ASSERT_MSG((expected) == (actual),                                   \
                    "expected %lu, got %lu",                                  \
                    (unsigned long)(expected), (unsigned long)(actual))

#define TEST_ASSERT_EQ_INT(expected, actual)                                  \
    TEST_ASSERT_MSG((expected) == (actual),                                   \
                    "expected %d, got %d",                                    \
                    (int)(expected), (int)(actual))

#define TEST_ASSERT_EQ_STR(expected, actual)                                  \
    TEST_ASSERT_MSG(strcmp((expected), (actual)) == 0,                        \
                    "expected \"%s\", got \"%s\"",                            \
                    (expected), (actual))

/* ------------------------------------------------------------------------- */
/* Test doubles for gateway config / metrics / events                        */
/* ------------------------------------------------------------------------- */
static const device_cfg_t k_test_devices[] = {
    {
        .addr_7bit = 0x58,
        .label = "psu_a",
        .poll_period_ms = 500,
        .status_period_ms = 10000,
    },
};

const char *g_profile_name = "test";
const config_t g_config = {
    .gw_id = "test_gw",
    .i2c = {
        .bus = 0,
        .speed_hz = 100000,
        .transaction_timeout_ms = 20,
        .retries = 2,
        .bus_recovery = true,
        .pec_enabled = true,
        .recovery_settle_ms = 5,
    },
    .mqtt = {
        .host = "127.0.0.1",
        .port = 1883,
        .client_id = "test",
        .base_topic = "pmbus/test",
        .qos_telemetry = 0,
        .qos_control = 1,
        .qos_metrics = 0,
        .backoff_min_ms = 500,
        .backoff_max_ms = 10000,
    },
    .buffer = {
        .enabled = true,
        .ram_max_records = 256,
        .flash_max_records = 0,
        .flush_batch_size = 50,
        .flush_interval_ms = 200,
        .drop_oldest = true,
    },
    .devices = k_test_devices,
    .num_devices = 1,
    .metrics_period_ms = 10000,
};

static event_type_t s_last_event_type;
static char s_last_event_detail[EVT_DETAIL_MAX];
static uint32_t s_event_count;
static uint32_t s_controller_reset_metrics;
static uint32_t s_bus_recovery_metrics;

static void reset_observers(void)
{
    s_last_event_type = (event_type_t)255;
    s_last_event_detail[0] = '\0';
    s_event_count = 0u;
    s_controller_reset_metrics = 0u;
    s_bus_recovery_metrics = 0u;
    i2c_mock_reset();
}

void gateway_ipc_post_event(event_type_t type, const char *detail)
{
    s_last_event_type = type;
    s_event_count++;
    if (detail != NULL)
    {
        strncpy(s_last_event_detail, detail, sizeof(s_last_event_detail) - 1u);
        s_last_event_detail[sizeof(s_last_event_detail) - 1u] = '\0';
    }
    else
    {
        s_last_event_detail[0] = '\0';
    }
}

void metrics_inc_i2c_controller_resets(void)
{
    s_controller_reset_metrics++;
}

void metrics_inc_i2c_bus_recoveries(void)
{
    s_bus_recovery_metrics++;
}

/* ------------------------------------------------------------------------- */
/* Test hook declarations (enabled via -DPMBUS_TEST_HOOKS)                   */
/* ------------------------------------------------------------------------- */
bool pmbus_test_should_attempt_bus_recovery(pmbus_status_t status);
bool pmbus_test_should_attempt_controller_reset(pmbus_status_t status);
bool pmbus_test_reset_controller_if_idle(const char *reason);

/* ------------------------------------------------------------------------- */
/* Tests                                                                     */
/* ------------------------------------------------------------------------- */
static void test_recovery_routing(void)
{
    printf("--- test_recovery_routing ---\n");

    TEST_ASSERT_TRUE(pmbus_test_should_attempt_controller_reset(PMBUS_ERR_TIMEOUT));
    TEST_ASSERT_TRUE(pmbus_test_should_attempt_controller_reset(PMBUS_ERR_NOT_READY));
    TEST_ASSERT_FALSE(pmbus_test_should_attempt_controller_reset(PMBUS_ERR_BUS_FAULT));
    TEST_ASSERT_FALSE(pmbus_test_should_attempt_controller_reset(PMBUS_ERR_NACK));

    TEST_ASSERT_TRUE(pmbus_test_should_attempt_bus_recovery(PMBUS_ERR_BUS_FAULT));
    TEST_ASSERT_FALSE(pmbus_test_should_attempt_bus_recovery(PMBUS_ERR_TIMEOUT));
    TEST_ASSERT_FALSE(pmbus_test_should_attempt_bus_recovery(PMBUS_ERR_NOT_READY));
    TEST_ASSERT_FALSE(pmbus_test_should_attempt_bus_recovery(PMBUS_ERR_NACK));
}

static void test_controller_reset_success(void)
{
    printf("--- test_controller_reset_success ---\n");
    reset_observers();

    i2c_mock_set_scl_level(1u);
    i2c_mock_set_sda_level(1u);
    i2c_mock_set_master_status(0u);

    TEST_ASSERT_TRUE(pmbus_test_reset_controller_if_idle("timeout"));
    TEST_ASSERT_EQ_U32(1u, i2c_mock_disable_calls());
    TEST_ASSERT_EQ_U32(1u, i2c_mock_enable_calls());
    TEST_ASSERT_EQ_U32(1u, s_event_count);
    TEST_ASSERT_EQ_INT(EVT_I2C_CONTROLLER_RESET, s_last_event_type);
    TEST_ASSERT_EQ_STR("timeout", s_last_event_detail);
    TEST_ASSERT_EQ_U32(1u, s_controller_reset_metrics);
    TEST_ASSERT_EQ_U32(0u, s_bus_recovery_metrics);
    TEST_ASSERT_EQ_U32(5u, i2c_mock_last_delay_ticks());
    TEST_ASSERT_EQ_U32(5u, i2c_mock_total_delay_ticks());
}

static void test_controller_reset_skip_when_bus_not_idle(void)
{
    uint32_t remaining_ms = 0u;

    printf("--- test_controller_reset_skip_when_bus_not_idle ---\n");
    reset_observers();

    i2c_mock_set_scl_level(0u);
    i2c_mock_set_sda_level(1u);

    TEST_ASSERT_FALSE(pmbus_test_reset_controller_if_idle("timeout"));
    TEST_ASSERT_EQ_U32(0u, i2c_mock_disable_calls());
    TEST_ASSERT_EQ_U32(0u, i2c_mock_enable_calls());
    TEST_ASSERT_EQ_U32(0u, s_event_count);
    TEST_ASSERT_EQ_U32(0u, s_controller_reset_metrics);
    TEST_ASSERT_EQ_U32(0u, i2c_mock_total_delay_ticks());

    TEST_ASSERT_TRUE(pmbus_bus_backoff_active(&remaining_ms));
    TEST_ASSERT_MSG(remaining_ms > 0u, "expected positive backoff, got %lu",
                    (unsigned long)remaining_ms);

    i2c_mock_set_scl_level(1u);
    i2c_mock_set_sda_level(1u);
    TEST_ASSERT_FALSE(pmbus_bus_backoff_active(&remaining_ms));
    TEST_ASSERT_EQ_U32(0u, remaining_ms);
}

static void test_bus_recovery_success(void)
{
    printf("--- test_bus_recovery_success ---\n");
    reset_observers();

    i2c_mock_set_sda_level(1u);
    i2c_mock_set_scl_hsiom(19u);

    TEST_ASSERT_EQ_INT(PMBUS_OK, pmbus_bus_recovery());
    TEST_ASSERT_EQ_U32(1u, i2c_mock_disable_calls());
    TEST_ASSERT_EQ_U32(1u, i2c_mock_enable_calls());
    TEST_ASSERT_EQ_U32(18u, i2c_mock_write_calls());
    TEST_ASSERT_EQ_U32(18u, i2c_mock_delay_us_calls());
    TEST_ASSERT_EQ_U32(1u, s_event_count);
    TEST_ASSERT_EQ_INT(EVT_PMBUS_BUS_RECOVERY, s_last_event_type);
    TEST_ASSERT_EQ_STR("", s_last_event_detail);
    TEST_ASSERT_EQ_U32(0u, s_controller_reset_metrics);
    TEST_ASSERT_EQ_U32(1u, s_bus_recovery_metrics);
    TEST_ASSERT_EQ_U32(5u, i2c_mock_last_delay_ticks());
    TEST_ASSERT_EQ_U32(5u, i2c_mock_total_delay_ticks());
}

static void test_bus_recovery_fail(void)
{
    printf("--- test_bus_recovery_fail ---\n");
    reset_observers();

    i2c_mock_set_sda_level(0u);

    TEST_ASSERT_EQ_INT(PMBUS_ERR_RECOVERY_FAIL, pmbus_bus_recovery());
    TEST_ASSERT_EQ_U32(1u, i2c_mock_disable_calls());
    TEST_ASSERT_EQ_U32(1u, i2c_mock_enable_calls());
    TEST_ASSERT_EQ_U32(18u, i2c_mock_write_calls());
    TEST_ASSERT_EQ_U32(18u, i2c_mock_delay_us_calls());
    TEST_ASSERT_EQ_U32(1u, s_event_count);
    TEST_ASSERT_EQ_INT(EVT_PMBUS_BUS_RECOVERY_FAIL, s_last_event_type);
    TEST_ASSERT_EQ_STR("SDA stuck low", s_last_event_detail);
    TEST_ASSERT_EQ_U32(0u, s_controller_reset_metrics);
    TEST_ASSERT_EQ_U32(0u, s_bus_recovery_metrics);
    TEST_ASSERT_EQ_U32(0u, i2c_mock_total_delay_ticks());
}

int main(void)
{
    printf("=== I2C Recovery Host Tests ===\n\n");

    test_recovery_routing();
    test_controller_reset_success();
    test_controller_reset_skip_when_bus_not_idle();
    test_bus_recovery_success();
    test_bus_recovery_fail();

    printf("\n=== Results: %d passed, %d failed, %d total ===\n",
           tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}
