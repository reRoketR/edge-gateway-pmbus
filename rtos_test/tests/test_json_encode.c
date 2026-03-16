/*******************************************************************************
 * File Name:   test_json_encode.c
 *
 * Description: Host-side unit tests for telemetry/status/event/metrics JSON
 *              encoding functions.
 *
 *              Compile and run on PC:
 *                gcc -o test_json_encode.exe test_json_encode.c
 *                    ../source/telemetry.c ../source/events.c
 *                    ../source/metrics.c ../source/gateway_config.c
 *                    ../source/pmbus_decode.c -lm -Wall -Wextra
 *                    -I../source -I../source/profiles
 *                ./test_json_encode.exe
 *
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../source/telemetry.h"
#include "../source/events.h"
#include "../source/metrics.h"
#include "../source/gateway_config.h"

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

#define TEST_ASSERT_TRUE(cond) \
    TEST_ASSERT_MSG(cond, "expected true, got false")

#define TEST_ASSERT_INT_EQ(expected, actual) \
    TEST_ASSERT_MSG((expected) == (actual), \
                    "expected %d, got %d", (int)(expected), (int)(actual))

/*******************************************************************************
 * Helper: check if JSON contains a substring
 ******************************************************************************/
static int json_contains(const char *json, const char *substr)
{
    return strstr(json, substr) != NULL;
}

/*******************************************************************************
 * Test: Telemetry JSON encoding
 ******************************************************************************/
static void test_telemetry_json(void)
{
    printf("--- test_telemetry_json ---\n");

    telemetry_record_t rec = {0};
    rec.ts_ms     = 1730000000000ULL;
    rec.time_synced = true;
    rec.seq       = 12345;
    rec.addr_7bit = 0x58;
    rec.label     = "psu_a";
    rec.pec       = true;
    rec.read_ms   = 7;
    rec.retries   = 0;

    rec.vin_mV    = 12030;   /* 12.03 V */
    rec.vout_mV   = 1020;    /* 1.02 V */
    rec.iin_mA    = 840;     /* 0.84 A */
    rec.iout_mA   = 5100;    /* 5.10 A */
    rec.temp1_mC  = 42500;   /* 42.5 °C */
    rec.pout_mW   = 5200;    /* 5.20 W */

    rec.raw_vout  = 0x0123;
    rec.valid_mask = TELEM_VALID_ALL;

    char buf[512];
    int len = encode_telemetry_json(&rec, buf, sizeof(buf));

    TEST_ASSERT_MSG(len > 0, "encode_telemetry_json returned %d", len);
    TEST_ASSERT_MSG((size_t)len == strlen(buf),
                    "len=%d but strlen=%d", len, (int)strlen(buf));

    printf("  JSON (%d bytes): %s\n", len, buf);

    /* Verify key fields present */
    TEST_ASSERT_TRUE(json_contains(buf, "\"ts_ms\":1730000000000"));
    TEST_ASSERT_TRUE(json_contains(buf, "\"time_synced\":true"));
    TEST_ASSERT_TRUE(json_contains(buf, "\"seq\":12345"));
    TEST_ASSERT_TRUE(json_contains(buf, "\"gw_id\":\"gw01\""));
    TEST_ASSERT_TRUE(json_contains(buf, "\"addr\":\"0x58\""));
    TEST_ASSERT_TRUE(json_contains(buf, "\"label\":\"psu_a\""));
    TEST_ASSERT_TRUE(json_contains(buf, "\"pec\":true"));
    TEST_ASSERT_TRUE(json_contains(buf, "\"read_ms\":7"));
    TEST_ASSERT_TRUE(json_contains(buf, "\"retries\":0"));

    /* Voltage group */
    TEST_ASSERT_TRUE(json_contains(buf, "\"vin\":12.03"));
    TEST_ASSERT_TRUE(json_contains(buf, "\"vout\":1.02"));

    /* Current group */
    TEST_ASSERT_TRUE(json_contains(buf, "\"iin\":0.84"));
    TEST_ASSERT_TRUE(json_contains(buf, "\"iout\":5.10"));

    /* Temperature */
    TEST_ASSERT_TRUE(json_contains(buf, "\"temp1\":42.5"));

    /* Power */
    TEST_ASSERT_TRUE(json_contains(buf, "\"pout\":5.20"));

    /* Raw */
    TEST_ASSERT_TRUE(json_contains(buf, "\"read_vout\":\"0x0123\""));
}

/*******************************************************************************
 * Test: Telemetry with partial validity
 ******************************************************************************/
static void test_telemetry_partial(void)
{
    printf("--- test_telemetry_partial ---\n");

    telemetry_record_t rec = {0};
    rec.ts_ms      = 1000;
    rec.time_synced = false;
    rec.seq        = 1;
    rec.addr_7bit  = 0x59;
    rec.label      = "psu_b";
    rec.pec        = false;
    rec.read_ms    = 3;
    rec.retries    = 1;
    rec.vout_mV    = 3300;
    rec.raw_vout   = 0xABCD;
    rec.valid_mask = TELEM_VALID_VOUT;  /* Only VOUT is valid */

    char buf[512];
    int len = encode_telemetry_json(&rec, buf, sizeof(buf));

    TEST_ASSERT_MSG(len > 0, "encode returned %d", len);
    printf("  JSON (%d bytes): %s\n", len, buf);

    /* VOUT present */
    TEST_ASSERT_TRUE(json_contains(buf, "\"vout\":3.30"));

    /* VIN, IIN, IOUT, TEMP1, POUT should NOT be present */
    TEST_ASSERT_TRUE(!json_contains(buf, "\"vin\""));
    TEST_ASSERT_TRUE(!json_contains(buf, "\"iin\""));
    TEST_ASSERT_TRUE(!json_contains(buf, "\"iout\""));
    TEST_ASSERT_TRUE(!json_contains(buf, "\"temp1\""));
    TEST_ASSERT_TRUE(!json_contains(buf, "\"pout\""));
}

/*******************************************************************************
 * Test: Telemetry buffer too small
 ******************************************************************************/
static void test_telemetry_buffer_too_small(void)
{
    printf("--- test_telemetry_buffer_too_small ---\n");

    telemetry_record_t rec = {0};
    rec.ts_ms      = 1000;
    rec.seq        = 1;
    rec.addr_7bit  = 0x58;
    rec.label      = "x";
    rec.valid_mask = TELEM_VALID_ALL;

    char buf[32];  /* Way too small */
    int len = encode_telemetry_json(&rec, buf, sizeof(buf));
    TEST_ASSERT_INT_EQ(-1, len);
}

/*******************************************************************************
 * Test: Status JSON encoding
 ******************************************************************************/
static void test_status_json(void)
{
    printf("--- test_status_json ---\n");

    status_record_t rec = {0};
    rec.ts_ms       = 1730000000000ULL;
    rec.time_synced = true;
    rec.seq         = 999;
    rec.addr_7bit   = 0x58;
    rec.label       = "psu_a";
    rec.status_word = 0x8040;
    rec.status_vout = 0x00;
    rec.status_iout = 0x12;
    rec.status_temperature = 0x00;
    rec.valid_mask  = STATUS_VALID_ALL;

    char buf[256];
    int len = encode_status_json(&rec, buf, sizeof(buf));

    TEST_ASSERT_MSG(len > 0, "encode_status_json returned %d", len);
    printf("  JSON (%d bytes): %s\n", len, buf);

    TEST_ASSERT_TRUE(json_contains(buf, "\"status_word\":\"0x8040\""));
    TEST_ASSERT_TRUE(json_contains(buf, "\"status_vout\":\"0x00\""));
    TEST_ASSERT_TRUE(json_contains(buf, "\"status_iout\":\"0x12\""));
    TEST_ASSERT_TRUE(json_contains(buf, "\"status_temperature\":\"0x00\""));
    TEST_ASSERT_TRUE(json_contains(buf, "\"gw_id\":\"gw01\""));
}

/*******************************************************************************
 * Test: Event JSON encoding
 ******************************************************************************/
static void test_event_json(void)
{
    printf("--- test_event_json ---\n");

    event_record_t evt = {0};
    evt.ts_ms = 1730000000000ULL;
    evt.time_synced = true;
    evt.type  = EVT_MQTT_DISCONNECTED;
    strncpy(evt.detail, "wifi_lost", sizeof(evt.detail));

    char buf[256];
    int len = encode_event_json(&evt, buf, sizeof(buf));

    TEST_ASSERT_MSG(len > 0, "encode_event_json returned %d", len);
    printf("  JSON (%d bytes): %s\n", len, buf);

    TEST_ASSERT_TRUE(json_contains(buf, "\"type\":\"MQTT_DISCONNECTED\""));
    TEST_ASSERT_TRUE(json_contains(buf, "\"time_synced\":true"));
    TEST_ASSERT_TRUE(json_contains(buf, "\"detail\":\"wifi_lost\""));
}

/*******************************************************************************
 * Test: Event type strings
 ******************************************************************************/
static void test_event_type_strings(void)
{
    printf("--- test_event_type_strings ---\n");

    TEST_ASSERT_MSG(strcmp(event_type_str(EVT_MQTT_CONNECTED), "MQTT_CONNECTED") == 0,
                    "EVT_MQTT_CONNECTED");
    TEST_ASSERT_MSG(strcmp(event_type_str(EVT_MQTT_DISCONNECTED), "MQTT_DISCONNECTED") == 0,
                    "EVT_MQTT_DISCONNECTED");
    TEST_ASSERT_MSG(strcmp(event_type_str(EVT_PMBUS_DEVICE_OFFLINE), "PMBUS_DEVICE_OFFLINE") == 0,
                    "EVT_PMBUS_DEVICE_OFFLINE");
    TEST_ASSERT_MSG(strcmp(event_type_str(EVT_PMBUS_BUS_RECOVERY), "PMBUS_BUS_RECOVERY") == 0,
                    "EVT_PMBUS_BUS_RECOVERY");
    TEST_ASSERT_MSG(strcmp(event_type_str(EVT_BUFFER_OVERFLOW), "BUFFER_OVERFLOW") == 0,
                    "EVT_BUFFER_OVERFLOW");
}

/*******************************************************************************
 * Test: Topic builders
 ******************************************************************************/
static void test_topic_builders(void)
{
    printf("--- test_topic_builders ---\n");

    char buf[128];

    /* Device telemetry topic */
    int len = build_device_topic(buf, sizeof(buf), 0x58, "telemetry");
    TEST_ASSERT_MSG(len > 0, "build_device_topic returned %d", len);
    TEST_ASSERT_MSG(strcmp(buf, "pmbus/gw01/dev/0x58/telemetry") == 0,
                    "got: %s", buf);

    /* Device status topic */
    len = build_device_topic(buf, sizeof(buf), 0x59, "status");
    TEST_ASSERT_MSG(strcmp(buf, "pmbus/gw01/dev/0x59/status") == 0,
                    "got: %s", buf);

    /* Events topic */
    len = build_events_topic(buf, sizeof(buf));
    TEST_ASSERT_MSG(len > 0, "build_events_topic returned %d", len);
    TEST_ASSERT_MSG(strcmp(buf, "pmbus/gw01/events") == 0,
                    "got: %s", buf);

    /* Metrics topic */
    len = build_metrics_topic(buf, sizeof(buf));
    TEST_ASSERT_MSG(len > 0, "build_metrics_topic returned %d", len);
    TEST_ASSERT_MSG(strcmp(buf, "pmbus/gw01/metrics") == 0,
                    "got: %s", buf);
}

/*******************************************************************************
 * Test: Metrics snapshot and JSON encoding
 ******************************************************************************/
static void test_metrics_json(void)
{
    printf("--- test_metrics_json ---\n");

    metrics_init();

    /* Simulate some activity */
    for (int i = 0; i < 100; i++)
    {
        metrics_inc_pmbus_reads_ok();
        metrics_record_pmbus_txn_us(5000 + i * 100);  /* 5.0..14.9 ms */
        metrics_record_read_to_publish_us(15000 + i * 200); /* 15..34.8 ms */
    }

    metrics_inc_pmbus_reads_fail();
    metrics_inc_pmbus_reads_fail();
    metrics_inc_pmbus_retries();
    metrics_inc_pmbus_retries();
    metrics_inc_pmbus_retries();
    metrics_inc_mqtt_pub_ok();

    for (int i = 0; i < 50; i++)
    {
        metrics_inc_mqtt_pub_ok();
    }

    metrics_set_buffer_depth_ram(42);
    metrics_set_wifi_rssi(-56);

    /* Take snapshot at t=2000 ms */
    metrics_snapshot_t snap;
    metrics_snapshot_and_reset(&snap, 2000, 2000);

    TEST_ASSERT_MSG(snap.counters.pmbus_reads_ok == 100,
                    "reads_ok=%u", (unsigned)snap.counters.pmbus_reads_ok);
    TEST_ASSERT_MSG(snap.counters.pmbus_reads_fail == 2,
                    "reads_fail=%u", (unsigned)snap.counters.pmbus_reads_fail);
    TEST_ASSERT_MSG(snap.counters.pmbus_retries == 3,
                    "retries=%u", (unsigned)snap.counters.pmbus_retries);
    TEST_ASSERT_MSG(snap.counters.mqtt_pub_ok == 51,
                    "mqtt_pub_ok=%u", (unsigned)snap.counters.mqtt_pub_ok);
    TEST_ASSERT_MSG(snap.gauges.buffer_depth_ram == 42,
                    "buffer_depth_ram=%u", (unsigned)snap.gauges.buffer_depth_ram);
    TEST_ASSERT_MSG(snap.gauges.wifi_rssi_dbm == -56,
                    "rssi=%d", (int)snap.gauges.wifi_rssi_dbm);

    /* Timing: avg of read_to_publish should be around 24900 us */
    TEST_ASSERT_MSG(snap.timing.read_to_publish_avg_us > 20000 &&
                    snap.timing.read_to_publish_avg_us < 30000,
                    "r2p_avg=%u", (unsigned)snap.timing.read_to_publish_avg_us);

    /* p95 should be near the 95th value */
    TEST_ASSERT_MSG(snap.timing.read_to_publish_p95_us > 30000,
                    "r2p_p95=%u", (unsigned)snap.timing.read_to_publish_p95_us);

    /* max = 15000 + 99*200 = 34800 us */
    TEST_ASSERT_MSG(snap.timing.read_to_publish_max_us == 34800,
                    "r2p_max=%u", (unsigned)snap.timing.read_to_publish_max_us);

    /* Encode to JSON */
    char buf[1024];
    int len = encode_metrics_json(&snap, buf, sizeof(buf));

    TEST_ASSERT_MSG(len > 0, "encode_metrics_json returned %d", len);
    printf("  JSON (%d bytes): %s\n", len, buf);

    TEST_ASSERT_TRUE(json_contains(buf, "\"pmbus_reads_ok\":100"));
    TEST_ASSERT_TRUE(json_contains(buf, "\"pmbus_reads_fail\":2"));
    TEST_ASSERT_TRUE(json_contains(buf, "\"buffer_depth_ram\":42"));
    TEST_ASSERT_TRUE(json_contains(buf, "\"wifi_rssi_dbm\":-56"));
    TEST_ASSERT_TRUE(json_contains(buf, "\"read_to_publish_avg\":"));
    TEST_ASSERT_TRUE(json_contains(buf, "\"read_to_publish_p95\":"));

    /* Verify counters were reset */
    metrics_snapshot_t snap2;
    metrics_snapshot_and_reset(&snap2, 4000, 4000);
    TEST_ASSERT_MSG(snap2.counters.pmbus_reads_ok == 0,
                    "after reset reads_ok=%u", (unsigned)snap2.counters.pmbus_reads_ok);
    TEST_ASSERT_MSG(snap2.window_ms == 2000,
                    "window_ms=%u", (unsigned)snap2.window_ms);
}

/*******************************************************************************
 * Main
 ******************************************************************************/
int main(void)
{
    printf("=== JSON Encoding & Metrics Unit Tests ===\n\n");

    test_telemetry_json();
    test_telemetry_partial();
    test_telemetry_buffer_too_small();
    test_status_json();
    test_event_json();
    test_event_type_strings();
    test_topic_builders();
    test_metrics_json();

    printf("\n=== Results: %d passed, %d failed, %d total ===\n",
           tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}
