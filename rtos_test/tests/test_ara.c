/*******************************************************************************
 * File Name:   test_ara.c
 *
 * Description: Host-side unit tests for pmbus_ara_read() — D2c-1.
 *
 * Verifies:
 *   - Correct byte parsing (rx >> 1)
 *   - Slave address = 0x0C
 *   - No I2C write prefix (bare read)
 *   - NACK is normal exit (not error)
 *   - No counter pollution on NACK
 *   - No bus recovery on NACK
 *   - Multi-value byte parsing
 *   - Timeout returns PMBUS_ERR_TIMEOUT
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
#include "cy_scb_i2c.h"
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

const char *g_profile_name = "test_ara";
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

/* Stub: event posting (pmbus_master.c references gateway_ipc_post_event) */
static uint32_t s_event_count;
void gateway_ipc_post_event(event_type_t type, const char *detail)
{
    (void)type;
    (void)detail;
    s_event_count++;
}

/* Metric stubs — track counters to assert no pollution */
static uint32_t s_reads_fail;
static uint32_t s_nack_count;
static uint32_t s_timeouts;
static uint32_t s_pec_fail;
static uint32_t s_controller_resets;
static uint32_t s_bus_recoveries;

void metrics_inc_pmbus_reads_ok(void)    {}
void metrics_inc_pmbus_reads_fail(void)  { s_reads_fail++; }
void metrics_inc_pmbus_retries(void)     {}
void metrics_inc_pmbus_timeouts(void)    { s_timeouts++; }
void metrics_inc_pmbus_nack(void)        { s_nack_count++; }
void metrics_inc_pmbus_pec_fail(void)    { s_pec_fail++; }
void metrics_inc_i2c_controller_resets(void) { s_controller_resets++; }
void metrics_inc_i2c_bus_recoveries(void)    { s_bus_recoveries++; }

static void reset_all(void)
{
    i2c_mock_reset();
    s_event_count = 0u;
    s_reads_fail = 0u;
    s_nack_count = 0u;
    s_timeouts = 0u;
    s_pec_fail = 0u;
    s_controller_resets = 0u;
    s_bus_recoveries = 0u;
}

/* ------------------------------------------------------------------------- */
/* Tests                                                                     */
/* ------------------------------------------------------------------------- */

static void test_ara_read_ok(void)
{
    printf("--- test_ara_read_ok ---\n");
    reset_all();
    pmbus_init();

    /* 0xB0 = (0x58 << 1) | 0 → addr 0x58 */
    uint8_t data[] = { 0xB0 };
    i2c_mock_set_read_data(data, 1);
    i2c_mock_set_master_status(0u);  /* transfer complete, no errors */

    uint8_t addr = 0;
    pmbus_status_t st = pmbus_ara_read(&addr);

    TEST_ASSERT_EQ_INT(PMBUS_OK, st);
    TEST_ASSERT_EQ_U32(0x58u, addr);

    pmbus_deinit();
}

static void test_ara_uses_slave_0x0C(void)
{
    printf("--- test_ara_uses_slave_0x0C ---\n");
    reset_all();
    pmbus_init();

    uint8_t data[] = { 0xB0 };
    i2c_mock_set_read_data(data, 1);
    i2c_mock_set_master_status(0u);

    uint8_t addr = 0;
    pmbus_ara_read(&addr);

    TEST_ASSERT_EQ_U32(0x0Cu, i2c_mock_last_read_slave_addr());

    pmbus_deinit();
}

static void test_ara_has_no_write_prefix(void)
{
    printf("--- test_ara_has_no_write_prefix ---\n");
    reset_all();
    pmbus_init();

    uint8_t data[] = { 0xB0 };
    i2c_mock_set_read_data(data, 1);
    i2c_mock_set_master_status(0u);

    uint8_t addr = 0;
    pmbus_ara_read(&addr);

    TEST_ASSERT_FALSE(i2c_mock_i2c_write_was_called_since_reset());

    pmbus_deinit();
}

static void test_ara_nack_is_normal_exit(void)
{
    printf("--- test_ara_nack_is_normal_exit ---\n");
    reset_all();
    pmbus_init();

    /* Simulate NACK response */
    i2c_mock_set_master_status(CY_SCB_I2C_MASTER_ADDR_NAK);

    uint8_t addr = 0xFF;
    pmbus_status_t st = pmbus_ara_read(&addr);

    TEST_ASSERT_EQ_INT(PMBUS_ERR_NACK, st);

    pmbus_deinit();
}

static void test_ara_nack_no_counter_pollution(void)
{
    printf("--- test_ara_nack_no_counter_pollution ---\n");
    reset_all();
    pmbus_init();

    i2c_mock_set_master_status(CY_SCB_I2C_MASTER_ADDR_NAK);

    uint8_t addr = 0xFF;
    pmbus_ara_read(&addr);

    TEST_ASSERT_EQ_U32(0u, s_reads_fail);
    TEST_ASSERT_EQ_U32(0u, s_nack_count);
    TEST_ASSERT_EQ_U32(0u, s_timeouts);
    TEST_ASSERT_EQ_U32(0u, s_pec_fail);

    pmbus_deinit();
}

static void test_ara_timeout_returns_timeout(void)
{
    printf("--- test_ara_timeout_returns_timeout ---\n");
    reset_all();
    pmbus_init();

    /* Keep BUSY flag set so wait_for_completion times out */
    i2c_mock_set_master_status(CY_SCB_I2C_MASTER_BUSY);

    uint8_t addr = 0xFF;
    pmbus_status_t st = pmbus_ara_read(&addr);

    TEST_ASSERT_EQ_INT(PMBUS_ERR_TIMEOUT, st);

    pmbus_deinit();
}

static void test_ara_timeout_no_bus_recovery(void)
{
    printf("--- test_ara_timeout_no_bus_recovery ---\n");
    reset_all();
    pmbus_init();

    /* Keep BUSY flag set so wait_for_completion times out */
    i2c_mock_set_master_status(CY_SCB_I2C_MASTER_BUSY);

    uint8_t addr = 0xFF;
    pmbus_status_t st = pmbus_ara_read(&addr);

    TEST_ASSERT_EQ_INT(PMBUS_ERR_TIMEOUT, st);
    /* No SCB disable/enable (no recovery) on ARA timeout */
    TEST_ASSERT_EQ_U32(0u, s_controller_resets);
    TEST_ASSERT_EQ_U32(0u, s_bus_recoveries);
    /* disable_calls = 0 means no Cy_SCB_I2C_Disable was called
     * (enable_calls starts at 1 from pmbus_init) */
    TEST_ASSERT_EQ_U32(0u, i2c_mock_disable_calls());

    pmbus_deinit();
}

static void test_ara_byte_parsing_multi_value(void)
{
    printf("--- test_ara_byte_parsing_multi_value ---\n");

    /* Test multiple raw byte → 7-bit address conversions */
    struct { uint8_t raw; uint8_t expected; } cases[] = {
        { 0x00, 0x00 },
        { 0x18, 0x0C },
        { 0xB0, 0x58 },
        { 0xFF, 0x7F },
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        reset_all();
        pmbus_init();

        uint8_t data[] = { cases[i].raw };
        i2c_mock_set_read_data(data, 1);
        i2c_mock_set_master_status(0u);

        uint8_t addr = 0xFF;
        pmbus_status_t st = pmbus_ara_read(&addr);

        TEST_ASSERT_EQ_INT(PMBUS_OK, st);
        TEST_ASSERT_EQ_U32(cases[i].expected, addr);

        pmbus_deinit();
    }
}

/* ------------------------------------------------------------------------- */
/* main                                                                      */
/* ------------------------------------------------------------------------- */
int main(void)
{
    printf("=== test_ara (D2c-1 ARA unit tests) ===\n\n");

    test_ara_read_ok();
    test_ara_uses_slave_0x0C();
    test_ara_has_no_write_prefix();
    test_ara_nack_is_normal_exit();
    test_ara_nack_no_counter_pollution();
    test_ara_timeout_returns_timeout();
    test_ara_timeout_no_bus_recovery();
    test_ara_byte_parsing_multi_value();

    printf("\n--- Results: %d passed, %d failed, %d total ---\n",
           tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}
