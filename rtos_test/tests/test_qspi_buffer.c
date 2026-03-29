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

static void fill_bytes(char *buf, size_t len, char value)
{
    for (size_t i = 0; i < len; i++)
    {
        buf[i] = value;
    }
}

static void test_normal_put_peek_consume(void)
{
    buffer_record_t rec;

    printf("--- test_normal_put_peek_consume ---\n");
    reset_and_init();

    TEST_ASSERT_TRUE(qspi_buffer_put("topic/a", "{\"v\":1}", 7u));
    TEST_ASSERT_EQ_U32(1u, qspi_buffer_depth());
    TEST_ASSERT_TRUE(qspi_buffer_peek(&rec));
    TEST_ASSERT_EQ_STR("topic/a", rec.topic);
    TEST_ASSERT_EQ_STR("{\"v\":1}", rec.payload);
    TEST_ASSERT_EQ_U32(7u, rec.payload_len);
    TEST_ASSERT_TRUE(qspi_buffer_consume());
    TEST_ASSERT_EQ_U32(0u, qspi_buffer_depth());
    TEST_ASSERT_EQ_U32(1u, qspi_mock_metric_buffer_enqueued());
    TEST_ASSERT_EQ_U32(1u, qspi_mock_metric_buffer_dequeued());
    TEST_ASSERT_EQ_U32(0u, qspi_mock_metric_buffer_dropped());
}

static void test_put_truncates_to_contract(void)
{
    buffer_record_t rec;
    char topic[BUFFER_TOPIC_MAX + 20u];
    char payload[BUFFER_PAYLOAD_MAX + 40u];

    printf("--- test_put_truncates_to_contract ---\n");
    reset_and_init();

    for (size_t i = 0; i < sizeof(topic) - 1u; i++)
    {
        topic[i] = (char)('a' + (i % 26u));
    }
    topic[sizeof(topic) - 1u] = '\0';
    fill_bytes(payload, sizeof(payload), 'P');

    TEST_ASSERT_TRUE(qspi_buffer_put(topic, payload, (uint16_t)sizeof(payload)));
    TEST_ASSERT_TRUE(qspi_buffer_peek(&rec));
    TEST_ASSERT_EQ_U32(BUFFER_TOPIC_MAX - 1u, (uint32_t)strlen(rec.topic));
    TEST_ASSERT_EQ_U32(BUFFER_PAYLOAD_MAX - 1u, rec.payload_len);
    TEST_ASSERT_EQ_U32(BUFFER_PAYLOAD_MAX - 1u, (uint32_t)strlen(rec.payload));
}

static void test_sector_boundary_crossing_erases_next_sector(void)
{
    char topic[BUFFER_TOPIC_MAX];
    char payload[BUFFER_PAYLOAD_MAX];
    uint32_t initial_erases;

    printf("--- test_sector_boundary_crossing_erases_next_sector ---\n");
    reset_and_init();

    memset(topic, 't', sizeof(topic) - 1u);
    topic[sizeof(topic) - 1u] = '\0';
    memset(payload, 'x', sizeof(payload) - 1u);
    payload[sizeof(payload) - 1u] = '\0';

    initial_erases = qspi_mock_erase_calls();
    while (qspi_mock_erase_calls() == initial_erases)
    {
        TEST_ASSERT_TRUE(qspi_buffer_put(topic, payload, BUFFER_PAYLOAD_MAX - 1u));
    }

    TEST_ASSERT_MSG(qspi_buffer_depth() > 0u, "expected non-empty buffer after boundary crossing");
}

static void test_ring_wraparound_stays_operational(void)
{
    char payload[BUFFER_PAYLOAD_MAX];
    char topic[32];
    buffer_record_t rec;
    uint32_t initial_erases;
    uint32_t guard = 0u;

    printf("--- test_ring_wraparound_stays_operational ---\n");
    reset_and_init();

    memset(payload, 'w', sizeof(payload) - 1u);
    payload[sizeof(payload) - 1u] = '\0';
    initial_erases = qspi_mock_erase_calls();

    while (qspi_mock_erase_calls() < (initial_erases + 6u))
    {
        snprintf(topic, sizeof(topic), "wrap/%lu", (unsigned long)guard);
        TEST_ASSERT_TRUE(qspi_buffer_put(topic, payload, BUFFER_PAYLOAD_MAX - 1u));
        guard++;
        TEST_ASSERT_MSG(guard < 4000u, "wraparound loop guard tripped at %lu",
                        (unsigned long)guard);
    }

    TEST_ASSERT_MSG(qspi_mock_metric_buffer_dropped() > 0u,
                    "expected drop metrics once ring wraps");
    TEST_ASSERT_TRUE(qspi_buffer_peek(&rec));
    TEST_ASSERT_MSG(rec.topic[0] != '\0', "expected a valid record after wraparound");
}

static void test_recovery_from_metadata_journal(void)
{
    buffer_record_t rec;

    printf("--- test_recovery_from_metadata_journal ---\n");
    reset_and_init();

    TEST_ASSERT_TRUE(qspi_buffer_put("rec/1", "alpha", 5u));
    TEST_ASSERT_TRUE(qspi_buffer_put("rec/2", "bravo", 5u));
    TEST_ASSERT_TRUE(qspi_buffer_consume());
    TEST_ASSERT_EQ_U32(1u, qspi_buffer_depth());

    TEST_ASSERT_TRUE(qspi_buffer_init());
    TEST_ASSERT_EQ_U32(1u, qspi_buffer_depth());
    TEST_ASSERT_TRUE(qspi_buffer_peek(&rec));
    TEST_ASSERT_EQ_STR("rec/2", rec.topic);
    TEST_ASSERT_EQ_STR("bravo", rec.payload);
}

static void test_corrupted_latest_metadata_falls_back(void)
{
    buffer_record_t rec;
    uint32_t latest_entry_offset = (uint32_t)(sizeof(qspi_meta_entry_t) * 2u);

    printf("--- test_corrupted_latest_metadata_falls_back ---\n");
    reset_and_init();

    TEST_ASSERT_TRUE(qspi_buffer_put("meta/1", "one", 3u));
    TEST_ASSERT_TRUE(qspi_buffer_put("meta/2", "two", 3u));
    TEST_ASSERT_EQ_U32(2u, qspi_buffer_depth());

    qspi_mock_corrupt_u32(latest_entry_offset + 24u, 0u);

    TEST_ASSERT_TRUE(qspi_buffer_init());
    TEST_ASSERT_EQ_U32(1u, qspi_buffer_depth());
    TEST_ASSERT_TRUE(qspi_buffer_peek(&rec));
    TEST_ASSERT_EQ_STR("meta/1", rec.topic);
    TEST_ASSERT_EQ_STR("one", rec.payload);
}

static void test_ping_pong_journal_rollover_recovers(void)
{
    char topic[24];
    uint32_t puts = 0u;
    uint32_t depth_before;
    uint32_t writes_before;
    buffer_record_t rec;
    const uint8_t *mmap;

    printf("--- test_ping_pong_journal_rollover_recovers ---\n");
    reset_and_init();

    while (puts < 9400u)
    {
        snprintf(topic, sizeof(topic), "jp/%lu", (unsigned long)puts);
        TEST_ASSERT_TRUE(qspi_buffer_put(topic, "x", 1u));
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
    TEST_ASSERT_MSG(rec.topic[0] != '\0', "expected valid record after rollover recovery");
}

int main(void)
{
    printf("=== QSPI Buffer Host Tests ===\n\n");

    test_normal_put_peek_consume();
    test_put_truncates_to_contract();
    test_sector_boundary_crossing_erases_next_sector();
    test_ring_wraparound_stays_operational();
    test_recovery_from_metadata_journal();
    test_corrupted_latest_metadata_falls_back();
    test_ping_pong_journal_rollover_recovers();

    printf("\n=== Results: %d passed, %d failed, %d total ===\n",
           tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}
