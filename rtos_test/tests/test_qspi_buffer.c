/*******************************************************************************
 * File Name:   test_qspi_buffer.c
 *
 * Description: Host-side unit tests for qspi_buffer.c using a RAM-backed
 *              QSPI flash mock.
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../source/qspi_buffer.h"
#include "qspi_mock.h"

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

#define TEST_ASSERT_TRUE(cond) TEST_ASSERT_MSG((cond), "expected true: %s", #cond)
#define TEST_ASSERT_FALSE(cond) TEST_ASSERT_MSG(!(cond), "expected false: %s", #cond)
#define TEST_ASSERT_EQ_U32(exp, act)                                          \
    TEST_ASSERT_MSG((uint32_t)(exp) == (uint32_t)(act),                       \
                    "expected %lu, got %lu",                                  \
                    (unsigned long)(uint32_t)(exp),                           \
                    (unsigned long)(uint32_t)(act))
#define TEST_ASSERT_EQ_STR(exp, act)                                          \
    TEST_ASSERT_MSG(strcmp((exp), (act)) == 0,                                \
                    "expected \"%s\", got \"%s\"",                            \
                    (exp), (act))

static void reset_and_init(void)
{
    qspi_mock_reset();
    TEST_ASSERT_TRUE(qspi_buffer_init());
}

static void test_geometry_constants(void)
{
    printf("--- test_geometry_constants ---\n");
    TEST_ASSERT_EQ_U32(256u, QSPI_FLASH_TOTAL_SECTORS);
    TEST_ASSERT_EQ_U32(2u, QSPI_BUF_JOURNAL_SECTORS);
    TEST_ASSERT_EQ_U32(1u, QSPI_BUF_RESERVED_TAIL_SECTORS);
    TEST_ASSERT_EQ_U32(255u, QSPI_BUF_TOTAL_SECTORS);
    TEST_ASSERT_EQ_U32(253u, QSPI_BUF_DATA_SECTORS);
    TEST_ASSERT_EQ_U32(262144u, QSPI_BUF_SECTOR_SIZE);
    TEST_ASSERT_EQ_U32(66846720u, QSPI_BUF_REGION_SIZE);
    TEST_ASSERT_EQ_U32(524288u, QSPI_BUF_DATA_START);
}

static buffer_record_t make_telem_record(uint8_t addr, uint32_t seq,
                                         uint32_t read_start_ms,
                                         uint32_t origin_boot_gen)
{
    telemetry_record_t src;
    buffer_record_t rec;

    memset(&src, 0, sizeof(src));
    src.ts_ms = 1000000u + seq;
    src.time_synced = true;
    src.seq = seq;
    src.addr_7bit = addr;
    src.label = "psu_a";
    src.pec = true;
    src.read_ms = 5u;
    src.retries = 1u;
    src.vin_mV = 12000 + (int32_t)seq;
    src.vout_mV = 1000u;
    src.iin_mA = 500;
    src.iout_mA = 4000;
    src.temp1_mC = 40000;
    src.pout_mW = 5000;
    src.raw_vin = 0x0101u;
    src.raw_vout = 0x0202u;
    src.raw_iin = 0x0303u;
    src.raw_iout = 0x0404u;
    src.raw_temp1 = 0x0505u;
    src.raw_pout = 0x0606u;
    src.valid_mask = TELEM_VALID_ALL;
    src.read_start_ms = read_start_ms;

    buffer_record_from_telemetry(&rec, &src, origin_boot_gen);
    return rec;
}

static buffer_record_t make_event_record(event_type_t type, const char *detail)
{
    event_record_t src;
    buffer_record_t rec;

    memset(&src, 0, sizeof(src));
    src.ts_ms = 2000000u;
    src.time_synced = true;
    src.type = type;
    if (detail != NULL)
    {
        strncpy(src.detail, detail, EVT_DETAIL_MAX - 1u);
        src.detail[EVT_DETAIL_MAX - 1u] = '\0';
    }

    buffer_record_from_event(&rec, &src);
    return rec;
}

static void test_roundtrip_telemetry_record(void)
{
    buffer_record_t in;
    buffer_record_t out;

    printf("--- test_roundtrip_telemetry_record ---\n");
    reset_and_init();

    in = make_telem_record(0x58u, 7u, 4321u, 99u);
    TEST_ASSERT_TRUE(qspi_buffer_put_record(&in));
    TEST_ASSERT_EQ_U32(1u, qspi_buffer_depth());
    TEST_ASSERT_TRUE(qspi_buffer_peek(&out));
    TEST_ASSERT_EQ_U32(BUFFER_RECORD_TELEMETRY, out.kind);
    TEST_ASSERT_EQ_U32(4321u, out.origin_read_start_ms);
    TEST_ASSERT_EQ_U32(99u, out.origin_boot_gen);
    TEST_ASSERT_EQ_U32(0x58u, out.payload.telemetry.addr_7bit);
    TEST_ASSERT_EQ_U32(7u, out.payload.telemetry.seq);
    TEST_ASSERT_EQ_U32(12007u, (uint32_t)out.payload.telemetry.vin_mV);
    TEST_ASSERT_EQ_U32(1000u, out.payload.telemetry.vout_mV);
    TEST_ASSERT_EQ_U32(5u, out.payload.telemetry.read_ms);
    TEST_ASSERT_EQ_U32(TELEM_VALID_ALL, out.payload.telemetry.valid_mask);
    TEST_ASSERT_TRUE(qspi_buffer_consume());
    TEST_ASSERT_EQ_U32(0u, qspi_buffer_depth());
    TEST_ASSERT_EQ_U32(1u, qspi_mock_metric_buffer_enqueued());
    TEST_ASSERT_EQ_U32(1u, qspi_mock_metric_buffer_dequeued());
    TEST_ASSERT_EQ_U32(0u, qspi_mock_metric_buffer_dropped());
}

static void test_invalid_record_is_rejected(void)
{
    buffer_record_t bad;

    printf("--- test_invalid_record_is_rejected ---\n");
    reset_and_init();

    memset(&bad, 0, sizeof(bad));
    bad.kind = 255u;

    TEST_ASSERT_FALSE(qspi_buffer_put_record(&bad));
    TEST_ASSERT_EQ_U32(0u, qspi_buffer_depth());
}

static void test_sector_boundary_crossing_erases_next_sector(void)
{
    buffer_record_t rec;
    buffer_record_t out;
    uint32_t initial_erases;
    uint32_t guard = 0u;

    printf("--- test_sector_boundary_crossing_erases_next_sector ---\n");
    reset_and_init();

    rec = make_event_record(EVT_MQTT_CONNECTED, "x");
    initial_erases = qspi_mock_erase_calls();
    while (qspi_mock_erase_calls() == initial_erases)
    {
        TEST_ASSERT_TRUE(qspi_buffer_put_record(&rec));
        guard++;
        TEST_ASSERT_MSG(guard < 5000u, "sector-crossing loop guard tripped at %lu",
                        (unsigned long)guard);
    }

    TEST_ASSERT_MSG(qspi_buffer_depth() > 0u,
                    "expected non-empty buffer after boundary crossing");
    TEST_ASSERT_TRUE(qspi_buffer_peek(&out));
    TEST_ASSERT_EQ_U32(BUFFER_RECORD_EVENT, out.kind);
    TEST_ASSERT_TRUE(strcmp(out.payload.event.detail, "x") == 0);
}

static void test_reinit_recovers_latest_record(void)
{
    buffer_record_t out;
    buffer_record_t rec1;
    buffer_record_t rec2;

    printf("--- test_reinit_recovers_latest_record ---\n");
    reset_and_init();

    rec1 = make_telem_record(0x58u, 1u, 100u, 11u);
    rec2 = make_telem_record(0x58u, 2u, 200u, 11u);
    TEST_ASSERT_TRUE(qspi_buffer_put_record(&rec1));
    TEST_ASSERT_TRUE(qspi_buffer_put_record(&rec2));
    TEST_ASSERT_TRUE(qspi_buffer_consume());
    TEST_ASSERT_EQ_U32(1u, qspi_buffer_depth());

    TEST_ASSERT_TRUE(qspi_buffer_init());
    TEST_ASSERT_EQ_U32(1u, qspi_buffer_depth());
    TEST_ASSERT_TRUE(qspi_buffer_peek(&out));
    TEST_ASSERT_EQ_U32(BUFFER_RECORD_TELEMETRY, out.kind);
    TEST_ASSERT_EQ_U32(2u, out.payload.telemetry.seq);
    TEST_ASSERT_EQ_U32(200u, out.origin_read_start_ms);
    TEST_ASSERT_EQ_U32(11u, out.origin_boot_gen);
    TEST_ASSERT_EQ_U32(0x58u, out.payload.telemetry.addr_7bit);
}

static void test_corrupted_latest_metadata_falls_back(void)
{
    buffer_record_t rec;
    uint32_t latest_entry_offset = (uint32_t)(sizeof(qspi_meta_entry_t) * 2u);

    printf("--- test_corrupted_latest_metadata_falls_back ---\n");
    reset_and_init();

    rec = make_event_record(EVT_MQTT_CONNECTED, "one");
    TEST_ASSERT_TRUE(qspi_buffer_put_record(&rec));
    rec = make_event_record(EVT_MQTT_CONNECTED, "two");
    TEST_ASSERT_TRUE(qspi_buffer_put_record(&rec));
    TEST_ASSERT_EQ_U32(2u, qspi_buffer_depth());

    qspi_mock_corrupt_u32(latest_entry_offset + 24u, 0u);

    TEST_ASSERT_TRUE(qspi_buffer_init());
    TEST_ASSERT_EQ_U32(1u, qspi_buffer_depth());
    TEST_ASSERT_TRUE(qspi_buffer_peek(&rec));
    TEST_ASSERT_EQ_U32(BUFFER_RECORD_EVENT, rec.kind);
    TEST_ASSERT_TRUE(strcmp(rec.payload.event.detail, "one") == 0);
}

static void test_ping_pong_journal_rollover_recovers(void)
{
    buffer_record_t rec;
    const uint8_t *mmap;
    uint32_t depth_before;
    uint32_t writes_before;
    uint32_t puts = 0u;

    printf("--- test_ping_pong_journal_rollover_recovers ---\n");
    reset_and_init();

    rec = make_event_record(EVT_MQTT_CONNECTED, "x");
    while (puts < 9400u)
    {
        TEST_ASSERT_TRUE(qspi_buffer_put_record(&rec));
        puts++;
    }

    mmap = qspi_mock_mmap_base();
    TEST_ASSERT_MSG(mmap[QSPI_BUF_JOURNAL_1_OFFSET] != 0xFFu,
                    "expected journal sector 1 to be populated after rollover");

    depth_before = qspi_buffer_depth();
    writes_before = qspi_buffer_total_writes();
    TEST_ASSERT_TRUE(qspi_buffer_init());
    TEST_ASSERT_EQ_U32(depth_before, qspi_buffer_depth());
    TEST_ASSERT_EQ_U32(writes_before, qspi_buffer_total_writes());
    TEST_ASSERT_TRUE(qspi_buffer_peek(&rec));
    TEST_ASSERT_EQ_U32(BUFFER_RECORD_EVENT, rec.kind);
}

int main(void)
{
    printf("=== QSPI Buffer Host Tests ===\n\n");

    test_geometry_constants();
    test_roundtrip_telemetry_record();
    test_invalid_record_is_rejected();
    test_sector_boundary_crossing_erases_next_sector();
    test_reinit_recovers_latest_record();
    test_corrupted_latest_metadata_falls_back();
    test_ping_pong_journal_rollover_recovers();

    printf("\n=== Results: %d passed, %d failed, %d total ===\n",
           tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}
