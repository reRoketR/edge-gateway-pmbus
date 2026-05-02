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

#define TEST_ASSERT_EQ_INT(exp, act)                                          \
    TEST_ASSERT_MSG((int)(exp) == (int)(act),                                 \
                    "expected %d, got %d",                                    \
                    (int)(exp), (int)(act))

#define TEST_ASSERT_EQ_U32(exp, act)                                          \
    TEST_ASSERT_MSG((uint32_t)(exp) == (uint32_t)(act),                       \
                    "expected %lu, got %lu",                                  \
                    (unsigned long)(uint32_t)(exp),                           \
                    (unsigned long)(uint32_t)(act))

static const device_cfg_t k_test_devices[] = {
    {
        .addr_7bit = 0x58,
        .label = "psu_a",
        .poll_period_ms = 500,
        .status_period_ms = 10000,
    },
};

const char *g_profile_name = "test_pmbus_generic_transfer";
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
        .drop_oldest = true,
    },
    .devices = k_test_devices,
    .num_devices = 1,
    .metrics_period_ms = 10000,
};

void gateway_ipc_post_event(event_type_t type, const char *detail)
{
    (void)type;
    (void)detail;
}

void metrics_inc_i2c_controller_resets(void) {}
void metrics_inc_i2c_bus_recoveries(void) {}

static void reset_bus(void)
{
    i2c_mock_reset();
    (void)pmbus_init();
    i2c_mock_set_master_status(0u);
    i2c_mock_set_write_result(CY_SCB_I2C_SUCCESS);
    i2c_mock_set_read_result(CY_SCB_I2C_SUCCESS);
}

static void finish_bus(void)
{
    pmbus_deinit();
}

static void test_write_only_uses_master_write(void)
{
    printf("--- test_write_only_uses_master_write ---\n");
    reset_bus();

    uint8_t wr[] = { 0x88, 0x12 };
    TEST_ASSERT_EQ_INT(PMBUS_OK,
        pmbus_generic_transfer(0x58, wr, sizeof(wr), NULL, 0u, false));

    TEST_ASSERT_EQ_U32(1u, i2c_mock_i2c_write_call_count());
    TEST_ASSERT_EQ_U32(0u, i2c_mock_i2c_read_call_count());
    TEST_ASSERT_EQ_U32(0x58u, i2c_mock_last_write_slave_addr());
    TEST_ASSERT_EQ_U32(2u, i2c_mock_last_write_len());
    TEST_ASSERT_FALSE(i2c_mock_last_write_xfer_pending());
    TEST_ASSERT_EQ_U32(0x88u, i2c_mock_last_write_byte(0u));
    TEST_ASSERT_EQ_U32(0x12u, i2c_mock_last_write_byte(1u));

    finish_bus();
}

static void test_bare_read_uses_master_read(void)
{
    printf("--- test_bare_read_uses_master_read ---\n");
    reset_bus();

    uint8_t data[] = { 0x34, 0x56 };
    uint8_t rd[2] = { 0u, 0u };
    i2c_mock_set_read_data(data, (uint8_t)sizeof(data));

    TEST_ASSERT_EQ_INT(PMBUS_OK,
        pmbus_generic_transfer(0x58, NULL, 0u, rd, sizeof(rd), false));

    TEST_ASSERT_EQ_U32(0u, i2c_mock_i2c_write_call_count());
    TEST_ASSERT_EQ_U32(1u, i2c_mock_i2c_read_call_count());
    TEST_ASSERT_EQ_U32(2u, i2c_mock_last_read_len());
    TEST_ASSERT_FALSE(i2c_mock_last_read_xfer_pending());
    TEST_ASSERT_EQ_U32(0x34u, rd[0]);
    TEST_ASSERT_EQ_U32(0x56u, rd[1]);

    finish_bus();
}

static void test_write_then_read_uses_repeated_start(void)
{
    printf("--- test_write_then_read_uses_repeated_start ---\n");
    reset_bus();

    uint8_t wr[] = { 0x88 };
    uint8_t rd[2] = { 0u, 0u };
    uint8_t read_payload[] = { 0x12, 0x34 };
    i2c_mock_set_read_data(read_payload, (uint8_t)sizeof(read_payload));

    TEST_ASSERT_EQ_INT(PMBUS_OK,
        pmbus_generic_transfer(0x58, wr, sizeof(wr), rd, sizeof(rd), false));

    TEST_ASSERT_EQ_U32(1u, i2c_mock_i2c_write_call_count());
    TEST_ASSERT_EQ_U32(1u, i2c_mock_i2c_read_call_count());
    TEST_ASSERT_TRUE(i2c_mock_last_write_xfer_pending());
    TEST_ASSERT_FALSE(i2c_mock_last_read_xfer_pending());
    TEST_ASSERT_EQ_U32(1u, i2c_mock_last_write_len());
    TEST_ASSERT_EQ_U32(0x12u, rd[0]);
    TEST_ASSERT_EQ_U32(0x34u, rd[1]);

    finish_bus();
}

static void test_invalid_shape_and_bounds(void)
{
    printf("--- test_invalid_shape_and_bounds ---\n");
    reset_bus();

    uint8_t wr_too_big[33] = { 0u };
    uint8_t rd_buf[33] = { 0u };

    TEST_ASSERT_EQ_INT(PMBUS_ERR_ARG,
        pmbus_generic_transfer(0x58, NULL, 0u, NULL, 0u, false));
    TEST_ASSERT_EQ_INT(PMBUS_ERR_ARG,
        pmbus_generic_transfer(0x58, wr_too_big, sizeof(wr_too_big),
                               NULL, 0u, false));
    TEST_ASSERT_EQ_INT(PMBUS_ERR_ARG,
        pmbus_generic_transfer(0x58, NULL, 0u, rd_buf, sizeof(rd_buf), false));

    finish_bus();
}

static void test_write_only_pec_appended(void)
{
    printf("--- test_write_only_pec_appended ---\n");
    reset_bus();

    uint8_t wr[] = { 0x88, 0x12 };
    uint8_t pec_input[] = { 0xB0, 0x88, 0x12 };
    uint8_t expected_pec = pmbus_crc8(pec_input, (uint8_t)sizeof(pec_input));

    TEST_ASSERT_EQ_INT(PMBUS_OK,
        pmbus_generic_transfer(0x58, wr, sizeof(wr), NULL, 0u, true));

    TEST_ASSERT_EQ_U32(3u, i2c_mock_last_write_len());
    TEST_ASSERT_EQ_U32(0x88u, i2c_mock_last_write_byte(0u));
    TEST_ASSERT_EQ_U32(0x12u, i2c_mock_last_write_byte(1u));
    TEST_ASSERT_EQ_U32(expected_pec, i2c_mock_last_write_byte(2u));

    finish_bus();
}

static void test_bare_read_pec_verified(void)
{
    printf("--- test_bare_read_pec_verified ---\n");
    reset_bus();

    uint8_t pec_input[] = { 0xB1, 0x34 };
    uint8_t payload[] = { 0x34, pmbus_crc8(pec_input, (uint8_t)sizeof(pec_input)) };
    uint8_t rd[1] = { 0u };
    i2c_mock_set_read_data(payload, (uint8_t)sizeof(payload));

    TEST_ASSERT_EQ_INT(PMBUS_OK,
        pmbus_generic_transfer(0x58, NULL, 0u, rd, sizeof(rd), true));
    TEST_ASSERT_EQ_U32(0x34u, rd[0]);

    finish_bus();
}

static void test_write_then_read_combined_pec_verified(void)
{
    printf("--- test_write_then_read_combined_pec_verified ---\n");
    reset_bus();

    uint8_t wr[] = { 0x88 };
    uint8_t pec_input[] = { 0xB0, 0x88, 0xB1, 0x12, 0x34 };
    uint8_t payload[] = {
        0x12,
        0x34,
        pmbus_crc8(pec_input, (uint8_t)sizeof(pec_input))
    };
    uint8_t rd[2] = { 0u, 0u };
    i2c_mock_set_read_data(payload, (uint8_t)sizeof(payload));

    TEST_ASSERT_EQ_INT(PMBUS_OK,
        pmbus_generic_transfer(0x58, wr, sizeof(wr), rd, sizeof(rd), true));
    TEST_ASSERT_EQ_U32(0x12u, rd[0]);
    TEST_ASSERT_EQ_U32(0x34u, rd[1]);

    finish_bus();
}

static void test_bad_pec_returns_error(void)
{
    printf("--- test_bad_pec_returns_error ---\n");
    reset_bus();

    uint8_t wr[] = { 0x88 };
    uint8_t payload[] = { 0x12, 0x34, 0x00 };
    uint8_t rd[2] = { 0u, 0u };
    i2c_mock_set_read_data(payload, (uint8_t)sizeof(payload));

    TEST_ASSERT_EQ_INT(PMBUS_ERR_PEC,
        pmbus_generic_transfer(0x58, wr, sizeof(wr), rd, sizeof(rd), true));

    finish_bus();
}

static void test_status_mapping_and_single_attempt(void)
{
    printf("--- test_status_mapping_and_single_attempt ---\n");

    reset_bus();
    i2c_mock_set_master_status(CY_SCB_I2C_MASTER_ADDR_NAK);
    {
        uint8_t wr[] = { 0x88 };
        TEST_ASSERT_EQ_INT(PMBUS_ERR_NACK,
            pmbus_generic_transfer(0x58, wr, sizeof(wr), NULL, 0u, false));
        TEST_ASSERT_EQ_U32(1u, i2c_mock_i2c_write_call_count());
    }
    finish_bus();

    reset_bus();
    i2c_mock_set_master_status(CY_SCB_I2C_MASTER_BUSY);
    {
        uint8_t rd[1] = { 0u };
        TEST_ASSERT_EQ_INT(PMBUS_ERR_TIMEOUT,
            pmbus_generic_transfer(0x58, NULL, 0u, rd, sizeof(rd), false));
        TEST_ASSERT_EQ_U32(1u, i2c_mock_i2c_read_call_count());
    }
    finish_bus();

    reset_bus();
    i2c_mock_set_master_status(CY_SCB_I2C_MASTER_BUS_ERR);
    {
        uint8_t rd[1] = { 0u };
        TEST_ASSERT_EQ_INT(PMBUS_ERR_BUS_FAULT,
            pmbus_generic_transfer(0x58, NULL, 0u, rd, sizeof(rd), false));
        TEST_ASSERT_EQ_U32(1u, i2c_mock_i2c_read_call_count());
    }
    finish_bus();

    reset_bus();
    i2c_mock_set_write_result(CY_SCB_I2C_MASTER_NOT_READY);
    {
        uint8_t wr[] = { 0x88 };
        TEST_ASSERT_EQ_INT(PMBUS_ERR_NOT_READY,
            pmbus_generic_transfer(0x58, wr, sizeof(wr), NULL, 0u, false));
        TEST_ASSERT_EQ_U32(1u, i2c_mock_i2c_write_call_count());
        TEST_ASSERT_EQ_U32(0u, i2c_mock_i2c_read_call_count());
    }
    finish_bus();
}

int main(void)
{
    printf("=== test_pmbus_generic_transfer ===\n\n");

    test_write_only_uses_master_write();
    test_bare_read_uses_master_read();
    test_write_then_read_uses_repeated_start();
    test_invalid_shape_and_bounds();
    test_write_only_pec_appended();
    test_bare_read_pec_verified();
    test_write_then_read_combined_pec_verified();
    test_bad_pec_returns_error();
    test_status_mapping_and_single_attempt();

    printf("\n=== Results: %d passed, %d failed, %d total ===\n",
           tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}
