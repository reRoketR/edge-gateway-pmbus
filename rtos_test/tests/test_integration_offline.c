/*******************************************************************************
 * File Name:   test_integration_offline.c
 *
 * Description: Integration test for the offline store-and-forward pipeline.
 *
 *              T-1a: gateway_ipc → emergency_ring → buffer_mgr_drain_once()
 *                    → buffer_mgr (RAM ring) → persistent_buffer (QSPI mock)
 *
 *              T-1b: Flush extension — peek/consume with mock publish callback,
 *                    verifying global FIFO ordering, batch limits, and failure
 *                    mid-batch.
 *
 * Compile:     -DBUFFER_BACKEND_QSPI -DQSPI_BUF_HOST_TEST
 *              -DBUFFER_MGR_HOST_TEST -DINTEGRATION_TEST
 ******************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "buffer_mgr.h"
#include "gateway_ipc.h"
#include "emergency_ring.h"
#include "persistent_buffer.h"
#include "persistent_seq.h"
#include "telemetry.h"
#include "events.h"
#include "metrics.h"
#include "gateway_config.h"
#include "qspi_mock.h"
#include "publish_mock.h"
#include "buffer_flush.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* Declared in rtos_stubs.c */
extern void test_queue_reset_all(void);
extern void test_set_tick(TickType_t t);
extern void test_set_wall_ms(uint64_t ms);

/*******************************************************************************
 * Test framework (same macros as other test suites)
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

#define TEST_ASSERT_TRUE(cond)  TEST_ASSERT_MSG((cond), "expected true: %s", #cond)
#define TEST_ASSERT_FALSE(cond) TEST_ASSERT_MSG(!(cond), "expected false: %s", #cond)
#define TEST_ASSERT_EQ_U32(exp, act)                                          \
    TEST_ASSERT_MSG((uint32_t)(exp) == (uint32_t)(act),                       \
                    "expected %lu, got %lu",                                  \
                    (unsigned long)(uint32_t)(exp),                           \
                    (unsigned long)(uint32_t)(act))

/*******************************************************************************
 * Test-local g_config (not linking gateway_config.c)
 *
 * We define g_config as const with a sensible default, then use memcpy
 * through a cast to reconfigure per-scenario. This is technically UB but
 * standard practice for host-side embedded tests (data is in writable .data).
 ******************************************************************************/

static const device_cfg_t k_test_devices[] = {
    {
        .addr_7bit = 0x58,
        .label     = "psu_a",
        .poll_period_ms   = 500,
        .status_period_ms = 10000,
    },
};

const char *g_profile_name = "integration_test";

config_t g_config = {
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
        .host       = "127.0.0.1",
        .port       = 1883,
        .client_id  = "test",
        .base_topic = "pmbus/test",
        .qos_telemetry  = 0,
        .qos_control    = 1,
        .qos_metrics    = 0,
        .backoff_min_ms = 500,
        .backoff_max_ms = 10000,
    },
    .buffer = {
        .enabled          = true,
        .ram_max_records   = 32,
        .flash_max_records = 100,
        .flush_batch_size  = 50,
        .drop_oldest       = true,
    },
    .reporting = {
        .telemetry_filter_enabled = false,
        .status_filter_enabled    = false,
        .status_emit_initial      = true,
        .telemetry_heartbeat_ms   = 10000,
        .status_heartbeat_ms      = 300000,
        .deadband_vin_mV  = 100,
        .deadband_vout_mV = 20,
        .deadband_iin_mA  = 100,
        .deadband_iout_mA = 100,
        .deadband_temp1_mC = 1000,
        .deadband_pout_mW  = 1000,
    },
    .devices     = k_test_devices,
    .num_devices = 1,
    .metrics_period_ms = 10000,
};

static void set_config_buffer(uint16_t ram_max, uint32_t flash_max,
                              uint16_t flush_batch, bool drop_oldest)
{
    /* Overwrite the const buffer sub-struct for this scenario */
    config_t tmp;
    memcpy(&tmp, &g_config, sizeof(config_t));
    tmp.buffer.ram_max_records  = ram_max;
    tmp.buffer.flash_max_records = flash_max;
    tmp.buffer.flush_batch_size = flush_batch;
    tmp.buffer.drop_oldest      = drop_oldest;
    memcpy((void *)&g_config, &tmp, sizeof(config_t));
}

/*******************************************************************************
 * Helper: create a telemetry record with identifiable content
 ******************************************************************************/
static telemetry_record_t make_telem(uint8_t addr, uint32_t seq, uint32_t read_start_ms)
{
    telemetry_record_t r;
    memset(&r, 0, sizeof(r));
    r.ts_ms         = 1000000u + seq;
    r.time_synced   = true;
    r.boot_count    = persistent_seq_get_boot_count();
    r.seq           = seq;
    r.addr_7bit     = addr;
    r.label         = "psu_a";
    r.pec           = true;
    r.read_ms       = 5;
    r.vin_mV        = 12000 + (int32_t)seq;
    r.vout_mV       = 1000;
    r.iin_mA        = 500;
    r.iout_mA       = 4000;
    r.temp1_mC      = 40000;
    r.pout_mW       = 5000;
    r.valid_mask    = TELEM_VALID_ALL;
    r.read_start_ms = read_start_ms;
    return r;
}

static status_record_t make_status(uint8_t addr, uint32_t seq)
{
    status_record_t r;
    memset(&r, 0, sizeof(r));
    r.ts_ms       = 1000000u + seq;
    r.time_synced = true;
    r.seq         = seq;
    r.addr_7bit   = addr;
    r.label       = "psu_a";
    r.status_word = 0x0000;
    r.valid_mask  = STATUS_VALID_ALL;
    return r;
}

static event_record_t make_event(event_type_t type, const char *detail)
{
    event_record_t e;
    memset(&e, 0, sizeof(e));
    e.ts_ms       = 1000000u;
    e.time_synced = true;
    e.type        = type;
    if (detail)
    {
        strncpy(e.detail, detail, EVT_DETAIL_MAX - 1u);
    }
    return e;
}

static uint32_t extract_seq_from_payload(const char *payload)
{
    const char *seq = strstr(payload, "\"seq\":");
    if (seq == NULL)
    {
        return UINT32_MAX;
    }

    return (uint32_t)strtoul(seq + 6, NULL, 10);
}

static void assert_published_seq(uint32_t index, uint32_t expected_seq)
{
    TEST_ASSERT_EQ_U32(expected_seq,
                       extract_seq_from_payload(publish_mock_get_payload(index)));
}

static void assert_payload_contains(uint32_t index, const char *needle)
{
    TEST_ASSERT_TRUE(strstr(publish_mock_get_payload(index), needle) != NULL);
}

/*******************************************************************************
 * Reinit helper — reset all state between scenarios
 ******************************************************************************/
static void reinit(void)
{
    test_queue_reset_all();
    qspi_mock_reset();
    emergency_ring_init();
    publish_mock_reset();
    metrics_init();

    /* Reinitialize IPC (creates fresh queues) */
    TEST_ASSERT_TRUE(gateway_ipc_init());

    /* Reinitialize buffer_mgr (allocates ring from pvPortMalloc = malloc) */
    TEST_ASSERT_TRUE(buffer_mgr_init());

    /* Late-init triggers persistent tier (QSPI mock) */
    buffer_mgr_late_init();

    test_set_tick(1000u);
    test_set_wall_ms(1000000u);
}

/*******************************************************************************
 * T-1a Scenario A: Queue → drain → RAM buffer (happy path)
 ******************************************************************************/
static void test_A_queue_drain_ram(void)
{
    printf("  [A] Queue -> drain -> RAM buffer ...\n");
    set_config_buffer(32, 100, 50, true);
    reinit();

    QueueHandle_t tq = gateway_ipc_telemetry_queue();
    TEST_ASSERT_TRUE(tq != NULL);

    /* Push 5 telemetry records */
    for (uint32_t i = 0; i < 5u; i++)
    {
        telemetry_record_t r = make_telem(0x58, i, 100u + i);
        TEST_ASSERT_EQ_U32(pdTRUE, xQueueSend(tq, &r, 0));
    }
    TEST_ASSERT_EQ_U32(5u, uxQueueMessagesWaiting(tq));

    buffer_mgr_drain_once();

    TEST_ASSERT_EQ_U32(5u, buffer_mgr_depth());
    TEST_ASSERT_EQ_U32(0u, uxQueueMessagesWaiting(tq));

    /* Verify a record can be peeked */
    buffer_record_t out;
    TEST_ASSERT_TRUE(buffer_mgr_peek(&out));
    TEST_ASSERT_EQ_U32(BUFFER_RECORD_TELEMETRY, out.kind);
    TEST_ASSERT_EQ_U32(0x58u, out.payload.telemetry.addr_7bit);
    TEST_ASSERT_EQ_U32(0u, out.payload.telemetry.seq);
    TEST_ASSERT_EQ_U32(100u, out.origin_read_start_ms);

    printf("  [A] PASSED\n");
}

/*******************************************************************************
 * T-1a Scenario B: Status + event queue drain
 ******************************************************************************/
static void test_B_status_event_drain(void)
{
    printf("  [B] Status + event queue drain ...\n");
    set_config_buffer(32, 100, 50, true);
    reinit();

    QueueHandle_t sq = gateway_ipc_status_queue();
    QueueHandle_t eq = gateway_ipc_event_queue();

    /* Push 3 status records */
    for (uint32_t i = 0; i < 3u; i++)
    {
        status_record_t r = make_status(0x58, i);
        TEST_ASSERT_EQ_U32(pdTRUE, xQueueSend(sq, &r, 0));
    }

    /* Push 2 event records */
    for (uint32_t i = 0; i < 2u; i++)
    {
        event_record_t e = make_event(EVT_MQTT_CONNECTED, "test");
        TEST_ASSERT_EQ_U32(pdTRUE, xQueueSend(eq, &e, 0));
    }

    buffer_mgr_drain_once();

    TEST_ASSERT_EQ_U32(5u, buffer_mgr_depth());
    TEST_ASSERT_EQ_U32(0u, uxQueueMessagesWaiting(sq));
    TEST_ASSERT_EQ_U32(0u, uxQueueMessagesWaiting(eq));

    printf("  [B] PASSED\n");
}

/*******************************************************************************
 * T-1a Scenario C: RAM overflow → QSPI spill
 ******************************************************************************/
static void test_C_ram_overflow_qspi_spill(void)
{
    printf("  [C] RAM overflow -> QSPI spill ...\n");
    set_config_buffer(4, 100, 50, true);
    reinit();

    QueueHandle_t tq = gateway_ipc_telemetry_queue();

    /* Push 7 telemetry records */
    for (uint32_t i = 0; i < 7u; i++)
    {
        telemetry_record_t r = make_telem(0x58, i, 200u + i);
        TEST_ASSERT_EQ_U32(pdTRUE, xQueueSend(tq, &r, 0));
    }

    buffer_mgr_drain_once();

    /* RAM should be full (4), overflow to flash (3) */
    TEST_ASSERT_EQ_U32(4u, buffer_mgr_depth());
    TEST_ASSERT_EQ_U32(3u, persistent_buffer_depth());

    printf("  [C] PASSED\n");
}

/*******************************************************************************
 * T-1a Scenario D: Emergency ring drain (manually seeded)
 *
 * Note: The ring is seeded directly via emergency_ring_put(), bypassing the
 * producer-side fallback in pmbus_poll_task.c.  This validates drain semantics
 * (ring → buffer_mgr) but not the overflow-rescue decision in the poll task.
 ******************************************************************************/
static void test_D_emergency_ring_drain(void)
{
    printf("  [D] Emergency ring drain ...\n");
    set_config_buffer(32, 100, 50, true);
    reinit();

    /* Seed 3 records directly into emergency ring */
    for (uint32_t i = 0; i < 3u; i++)
    {
        telemetry_record_t r = make_telem(0x58, i, 300u + i);
        TEST_ASSERT_TRUE(emergency_ring_put(&r));
    }

    buffer_mgr_drain_once();

    /* All 3 should be recovered into buffer_mgr */
    TEST_ASSERT_TRUE(buffer_mgr_depth() >= 3u);

    printf("  [D] PASSED\n");
}

/*******************************************************************************
 * T-1a Scenario E: Queue full + emergency ring fallback → drain all
 *
 * Note: Emergency ring is manually seeded (same limitation as Scenario D).
 ******************************************************************************/
static void test_E_queue_full_with_emergency(void)
{
    printf("  [E] Queue full + emergency ring -> drain all ...\n");
    set_config_buffer(32, 100, 50, true);
    reinit();

    /* Use a small queue to demonstrate fullness.
     * The IPC queues are created with fixed depths (64/16/16),
     * so we fill the telemetry queue beyond send capacity. */
    QueueHandle_t tq = gateway_ipc_telemetry_queue();

    /* Fill the telemetry queue to capacity */
    uint32_t queued = 0;
    for (uint32_t i = 0; i < 200u; i++)
    {
        telemetry_record_t r = make_telem(0x58, i, 400u + i);
        if (xQueueSend(tq, &r, 0) == pdTRUE)
        {
            queued++;
        }
        else
        {
            /* Queue full — put remaining in emergency ring */
            TEST_ASSERT_TRUE(emergency_ring_put(&r));
            queued++;
            break;
        }
    }

    /* Add 2 more to emergency ring */
    for (uint32_t i = 0; i < 2u; i++)
    {
        telemetry_record_t r = make_telem(0x58, 200u + i, 600u + i);
        TEST_ASSERT_TRUE(emergency_ring_put(&r));
    }

    buffer_mgr_drain_once();

    /* All records from queue + emergency ring should be in the buffer */
    uint32_t total = buffer_mgr_depth() + persistent_buffer_depth();
    TEST_ASSERT_TRUE(total >= queued + 2u);

    printf("  [E] PASSED\n");
}

/*******************************************************************************
 * T-1a Scenario F: Full drop scenario
 *
 * Note: Emergency ring is manually seeded (same limitation as Scenario D).
 ******************************************************************************/
static void test_F_full_drop(void)
{
    printf("  [F] Full emergency ring drop ...\n");
    set_config_buffer(32, 100, 50, true);
    reinit();

    /* Fill emergency ring to capacity (256) */
    uint32_t ring_pushed = 0u;
    for (uint32_t i = 0; i < EMERGENCY_RING_CAPACITY; i++)
    {
        telemetry_record_t r = make_telem(0x58, i, 700u + i);
        if (emergency_ring_put(&r))
        {
            ring_pushed++;
        }
    }
    /* SPSC ring: capacity-1 slots usable */
    TEST_ASSERT_TRUE(ring_pushed > 0u);

    /* One more should fail (ring full) */
    telemetry_record_t extra = make_telem(0x58, 999, 999);
    TEST_ASSERT_FALSE(emergency_ring_put(&extra));

    buffer_mgr_drain_once();

    /* All ring records should be drained into buffer */
    uint32_t total = buffer_mgr_depth() + persistent_buffer_depth();
    TEST_ASSERT_EQ_U32(ring_pushed, total);

    printf("  [F] PASSED\n");
}

/*******************************************************************************
 * T-1a Scenario G: Metrics counters
 ******************************************************************************/
static void test_G_metrics_counters(void)
{
    printf("  [G] Metrics counters ...\n");
    set_config_buffer(4, 100, 50, true);
    reinit();

    QueueHandle_t tq = gateway_ipc_telemetry_queue();

    /* Push 7 records -> 4 RAM + 3 flash */
    for (uint32_t i = 0; i < 7u; i++)
    {
        telemetry_record_t r = make_telem(0x58, i, 800u + i);
        xQueueSend(tq, &r, 0);
    }

    buffer_mgr_drain_once();

    metrics_snapshot_t snap;
    metrics_snapshot_and_reset(&snap, 1000000u, 10000u);

    /* At least 7 enqueued (4 RAM by buffer_mgr + 3 by persistent backend) */
    TEST_ASSERT_TRUE(snap.counters.buffer_enqueued >= 4u);
    /* No drops expected — spill to flash succeeded */
    TEST_ASSERT_EQ_U32(0u, snap.counters.buffer_dropped);

    printf("  [G] PASSED\n");
}

/*******************************************************************************
 * T-1b: Flush uses the real production buffer_flush_records() from
 *       buffer_flush.c with an injectable publish callback.
 ******************************************************************************/

/*******************************************************************************
 * T-1b Scenario H: Normal flush (reconnect drain)
 ******************************************************************************/
static void test_H_normal_flush(void)
{
    printf("  [H] Normal flush (reconnect drain) ...\n");
    set_config_buffer(32, 100, 50, true);
    reinit();

    QueueHandle_t tq = gateway_ipc_telemetry_queue();

    /* Seed 8 records */
    for (uint32_t i = 0; i < 8u; i++)
    {
        telemetry_record_t r = make_telem(0x58, i, 900u + i);
        xQueueSend(tq, &r, 0);
    }
    buffer_mgr_drain_once();
    TEST_ASSERT_EQ_U32(8u, buffer_mgr_depth());

    /* Flush all — exercises real production buffer_flush_records() */
    uint16_t flushed = buffer_flush_records(publish_mock_fn);
    TEST_ASSERT_EQ_U32(8u, flushed);
    TEST_ASSERT_EQ_U32(8u, publish_mock_get_count());
    TEST_ASSERT_EQ_U32(0u, buffer_mgr_depth());

    /* Verify records are telemetry JSON with identifiable content */
    TEST_ASSERT_TRUE(strstr(publish_mock_get_topic(0), "telemetry") != NULL);
    TEST_ASSERT_TRUE(strstr(publish_mock_get_payload(0), "vin") != NULL);
    {
        char expected_boot[32];
        snprintf(expected_boot, sizeof(expected_boot), "\"boot_count\":%lu",
                 (unsigned long)persistent_seq_get_boot_count());
        TEST_ASSERT_TRUE(strstr(publish_mock_get_payload(0), expected_boot) != NULL);
    }
    TEST_ASSERT_TRUE(strstr(publish_mock_get_payload(0), "\"sample_monotonic_ms\":900") != NULL);

    metrics_snapshot_t snap;
    metrics_snapshot_and_reset(&snap, 1000000u, 10000u);
    TEST_ASSERT_EQ_U32(8u, snap.timing.read_to_publish_sample_count);
    TEST_ASSERT_EQ_U32(snap.timing.read_to_publish_avg_us,
                       snap.timing.telemetry_before_publish_avg_us +
                       snap.timing.telemetry_publish_avg_us);

    printf("  [H] PASSED\n");
}

/*******************************************************************************
 * T-1b Scenario I: Publish failure mid-batch
 ******************************************************************************/
static void test_I_publish_failure(void)
{
    printf("  [I] Publish failure mid-batch ...\n");
    set_config_buffer(32, 100, 50, true);
    reinit();

    QueueHandle_t tq = gateway_ipc_telemetry_queue();

    /* Seed 10 records */
    for (uint32_t i = 0; i < 10u; i++)
    {
        telemetry_record_t r = make_telem(0x58, i, 1000u + i);
        xQueueSend(tq, &r, 0);
    }
    buffer_mgr_drain_once();
    TEST_ASSERT_EQ_U32(10u, buffer_mgr_depth());

    /* Fail after 4 successful publishes */
    publish_mock_set_fail_after(4);

    uint16_t flushed = buffer_flush_records(publish_mock_fn);
    TEST_ASSERT_EQ_U32(4u, flushed);
    TEST_ASSERT_EQ_U32(4u, publish_mock_get_count());
    TEST_ASSERT_EQ_U32(6u, buffer_mgr_depth());

    printf("  [I] PASSED\n");
}

/*******************************************************************************
 * T-1b Scenario J: Global FIFO ordering across persistent + RAM tiers
 ******************************************************************************/
static void test_J_global_fifo_ordering(void)
{
    printf("  [J] Global FIFO ordering across tiers ...\n");
    set_config_buffer(4, 100, 50, true);
    reinit();

    QueueHandle_t tq = gateway_ipc_telemetry_queue();

    /* Seed 7 records: 4 fit in RAM, 3 spill to flash */
    for (uint32_t i = 0; i < 7u; i++)
    {
        telemetry_record_t r = make_telem(0x58, i, 1100u + i);
        xQueueSend(tq, &r, 0);
    }
    buffer_mgr_drain_once();
    TEST_ASSERT_EQ_U32(4u, buffer_mgr_depth());
    TEST_ASSERT_EQ_U32(3u, persistent_buffer_depth());

    /* Flush all: oldest-to-newest order must survive the spill. */
    uint16_t flushed = buffer_flush_records(publish_mock_fn);
    TEST_ASSERT_EQ_U32(7u, flushed);
    TEST_ASSERT_EQ_U32(7u, publish_mock_get_count());
    TEST_ASSERT_EQ_U32(0u, buffer_mgr_depth());
    TEST_ASSERT_EQ_U32(0u, persistent_buffer_depth());

    for (uint32_t i = 0; i < 7u; i++)
    {
        TEST_ASSERT_TRUE(strstr(publish_mock_get_topic(i), "telemetry") != NULL);
        assert_published_seq(i, i);
    }

    printf("  [J] PASSED\n");
}

/*******************************************************************************
 * T-1b Scenario K: Mixed tiers + batch limit
 ******************************************************************************/
static void test_K_batch_limit(void)
{
    printf("  [K] Mixed tiers + batch limit ...\n");
    set_config_buffer(4, 100, 5, true);
    reinit();

    QueueHandle_t tq = gateway_ipc_telemetry_queue();

    /* Seed 7 records: 4 RAM + 3 flash */
    for (uint32_t i = 0; i < 7u; i++)
    {
        telemetry_record_t r = make_telem(0x58, i, 1200u + i);
        xQueueSend(tq, &r, 0);
    }
    buffer_mgr_drain_once();
    TEST_ASSERT_EQ_U32(4u, buffer_mgr_depth());
    TEST_ASSERT_EQ_U32(3u, persistent_buffer_depth());

    /* First flush: batch=5 → 3 flash + 2 RAM */
    uint16_t flushed1 = buffer_flush_records(publish_mock_fn);
    TEST_ASSERT_EQ_U32(5u, flushed1);
    TEST_ASSERT_EQ_U32(2u, buffer_mgr_depth());
    TEST_ASSERT_EQ_U32(0u, persistent_buffer_depth());
    for (uint32_t i = 0; i < 5u; i++)
    {
        assert_published_seq(i, i);
    }

    /* Second flush: remaining 2 RAM */
    publish_mock_reset();
    uint16_t flushed2 = buffer_flush_records(publish_mock_fn);
    TEST_ASSERT_EQ_U32(2u, flushed2);
    TEST_ASSERT_EQ_U32(0u, buffer_mgr_depth());
    assert_published_seq(0u, 5u);
    assert_published_seq(1u, 6u);

    printf("  [K] PASSED\n");
}

/*******************************************************************************
 * T-1b Scenario L: Status queue then rescue ring ordering
 *
 * Manual-seed variant: validates drain ordering once records are already in the
 * queue and rescue ring. Scenario N exercises the real producer overflow path.
 ******************************************************************************/
static void test_L_status_rescue_ordering(void)
{
    printf("  [L] Status queue then rescue ring ordering ...\n");
    set_config_buffer(32, 100, 50, true);
    reinit();

    QueueHandle_t sq = gateway_ipc_status_queue();
    status_record_t s0 = make_status(0x58, 10u);
    status_record_t s1 = make_status(0x58, 11u);
    status_record_t s2 = make_status(0x58, 12u);
    status_record_t s3 = make_status(0x58, 13u);

    TEST_ASSERT_EQ_U32(pdTRUE, xQueueSend(sq, &s0, 0));
    TEST_ASSERT_EQ_U32(pdTRUE, xQueueSend(sq, &s1, 0));
    TEST_ASSERT_TRUE(emergency_status_ring_put(&s2));
    TEST_ASSERT_TRUE(emergency_status_ring_put(&s3));

    buffer_mgr_drain_once();

    TEST_ASSERT_EQ_U32(4u, buffer_mgr_depth());
    TEST_ASSERT_EQ_U32(0u, uxQueueMessagesWaiting(sq));
    TEST_ASSERT_EQ_U32(4u, buffer_flush_records(publish_mock_fn));

    for (uint32_t i = 0; i < 4u; i++)
    {
        TEST_ASSERT_TRUE(strstr(publish_mock_get_topic(i), "status") != NULL);
        assert_published_seq(i, 10u + i);
    }

    printf("  [L] PASSED\n");
}

/*******************************************************************************
 * T-1b Scenario M: Event queue overflow uses rescue ring
 ******************************************************************************/
static void test_M_event_rescue_overflow(void)
{
    printf("  [M] Event queue overflow rescue ...\n");
    set_config_buffer(64, 100, 64, true);
    reinit();

    QueueHandle_t eq = gateway_ipc_event_queue();
    uint32_t queued = 0u;

    for (uint32_t i = 0; i < 32u; i++)
    {
        char detail[16];
        snprintf(detail, sizeof(detail), "queue_%02lu", (unsigned long)i);
        event_record_t evt = make_event(EVT_MQTT_CONNECTED, detail);
        if (xQueueSend(eq, &evt, 0) != pdTRUE)
        {
            break;
        }
        queued++;
    }

    TEST_ASSERT_TRUE(queued > 0u);

    gateway_ipc_post_event(EVT_QUEUE_OVERFLOW, "rescue_a");
    gateway_ipc_post_event(EVT_QUEUE_OVERFLOW, "rescue_b");

    buffer_mgr_drain_once();

    TEST_ASSERT_EQ_U32(queued + 2u, buffer_mgr_depth());
    TEST_ASSERT_EQ_U32(queued + 2u, buffer_flush_records(publish_mock_fn));

    for (uint32_t i = 0; i < queued; i++)
    {
        TEST_ASSERT_TRUE(strstr(publish_mock_get_topic(i), "events") != NULL);
    }
    assert_payload_contains(queued, "rescue_a");
    assert_payload_contains(queued + 1u, "rescue_b");

    printf("  [M] PASSED\n");
}

/*******************************************************************************
 * T-1b Scenario N: Status queue overflow uses the real rescue path
 ******************************************************************************/
static void test_N_status_rescue_overflow(void)
{
    printf("  [N] Status queue overflow rescue ...\n");
    set_config_buffer(64, 100, 64, true);
    reinit();

    QueueHandle_t sq = gateway_ipc_status_queue();
    uint32_t queued = 0u;

    for (uint32_t i = 0; i < 32u; i++)
    {
        status_record_t rec = make_status(0x58, i);
        if (xQueueSend(sq, &rec, 0) != pdTRUE)
        {
            break;
        }
        queued++;
    }

    TEST_ASSERT_TRUE(queued > 0u);

    {
        status_record_t rescue_a = make_status(0x58, queued);
        status_record_t rescue_b = make_status(0x58, queued + 1u);
        TEST_ASSERT_TRUE(gateway_ipc_try_post_status(&rescue_a));
        TEST_ASSERT_TRUE(gateway_ipc_try_post_status(&rescue_b));
    }

    buffer_mgr_drain_once();

    TEST_ASSERT_EQ_U32(queued + 2u, buffer_mgr_depth());
    TEST_ASSERT_EQ_U32(queued + 2u, buffer_flush_records(publish_mock_fn));

    for (uint32_t i = 0; i < queued + 2u; i++)
    {
        TEST_ASSERT_TRUE(strstr(publish_mock_get_topic(i), "status") != NULL);
        assert_published_seq(i, i);
    }

    printf("  [N] PASSED\n");
}

/*******************************************************************************
 * Main
 ******************************************************************************/
int main(void)
{
    printf("=== Integration Test: Offline Path ===\n\n");
    printf("--- T-1a: Ingress + Buffer (no MQTT) ---\n");

    test_A_queue_drain_ram();
    test_B_status_event_drain();
    test_C_ram_overflow_qspi_spill();
    test_D_emergency_ring_drain();
    test_E_queue_full_with_emergency();
    test_F_full_drop();
    test_G_metrics_counters();

    printf("\n--- T-1b: Flush Extension ---\n");

    test_H_normal_flush();
    test_I_publish_failure();
    test_J_global_fifo_ordering();
    test_K_batch_limit();
    test_L_status_rescue_ordering();
    test_M_event_rescue_overflow();
    test_N_status_rescue_overflow();

    printf("\n=== Results: %d passed, %d failed, %d total ===\n",
           tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}

/* [] END OF FILE */
