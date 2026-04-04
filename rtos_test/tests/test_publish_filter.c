/*******************************************************************************
 * File Name:   test_publish_filter.c
 *
 * Description: Host-side unit tests for the publish-filter logic
 *              (telemetry deadband + heartbeat, status on-change + heartbeat).
 *
 *              Compile and run on PC:
 *                gcc -o test_publish_filter.exe test_publish_filter.c
 *                    ../source/publish_filter.c ../source/gateway_config.c
 *                    -Wall -Wextra -Itests/stubs -I../source -I../source/profiles
 *                ./test_publish_filter.exe
 *
 ******************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "../source/publish_filter.h"
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

#define TEST_ASSERT_FALSE(cond) \
    TEST_ASSERT_MSG(!(cond), "expected false, got true")

/*******************************************************************************
 * Helper: build a config_t with filtering enabled and known defaults
 ******************************************************************************/
static config_t make_test_config(bool telem_en, bool status_en)
{
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.reporting.telemetry_filter_enabled = telem_en;
    cfg.reporting.status_filter_enabled    = status_en;
    cfg.reporting.status_emit_initial      = true;
    cfg.reporting.telemetry_heartbeat_ms   = 10000;
    cfg.reporting.status_heartbeat_ms      = 300000;
    cfg.reporting.deadband_vin_mV          = 100;
    cfg.reporting.deadband_vout_mV         = 20;
    cfg.reporting.deadband_iin_mA          = 100;
    cfg.reporting.deadband_iout_mA         = 100;
    cfg.reporting.deadband_temp1_mC        = 1000;
    cfg.reporting.deadband_pout_mW         = 1000;
    return cfg;
}

static device_cfg_t make_test_dev(void)
{
    device_cfg_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.addr_7bit       = 0x58;
    dev.label           = "psu_a";
    dev.poll_period_ms  = 500;
    dev.status_period_ms = 10000;
    return dev;
}

/*******************************************************************************
 * Telemetry filter tests
 ******************************************************************************/

static void test_telem_first_sample_always_emitted(void)
{
    printf("--- test_telem_first_sample_always_emitted ---\n");
    config_t cfg = make_test_config(true, true);
    device_cfg_t dev = make_test_dev();
    telem_filter_state_t fs;
    memset(&fs, 0, sizeof(fs));
    fs.have_last = false;

    telemetry_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.valid_mask = TELEM_VALID_ALL;
    rec.vin_mV = 12000;

    TEST_ASSERT_TRUE(pf_should_emit_telemetry(&rec, &dev, &cfg, &fs, 0));
}

static void test_telem_filter_disabled_always_emits(void)
{
    printf("--- test_telem_filter_disabled_always_emits ---\n");
    config_t cfg = make_test_config(false, true);
    device_cfg_t dev = make_test_dev();
    telem_filter_state_t fs;
    memset(&fs, 0, sizeof(fs));
    fs.have_last = true;
    fs.last_emit_tick = 0;
    fs.last_valid_mask = TELEM_VALID_ALL;
    fs.last_vin_mV = 12000;

    telemetry_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.valid_mask = TELEM_VALID_ALL;
    rec.vin_mV = 12000;  /* identical — would be suppressed if enabled */

    /* tick=1 so heartbeat not expired yet */
    TEST_ASSERT_TRUE(pf_should_emit_telemetry(&rec, &dev, &cfg, &fs, 1));
}

static void test_telem_identical_suppressed(void)
{
    printf("--- test_telem_identical_suppressed ---\n");
    config_t cfg = make_test_config(true, true);
    device_cfg_t dev = make_test_dev();
    telem_filter_state_t fs;
    memset(&fs, 0, sizeof(fs));

    telemetry_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.valid_mask = TELEM_VALID_ALL;
    rec.vin_mV  = 12000;
    rec.vout_mV = 1000;
    rec.iin_mA  = 500;
    rec.iout_mA = 2000;
    rec.temp1_mC = 35000;
    rec.pout_mW  = 2000;

    /* Emit first sample and advance baseline */
    pf_advance_telem_baseline(&rec, &fs, 0);

    /* Same values, tick=1 (well within heartbeat) */
    TEST_ASSERT_FALSE(pf_should_emit_telemetry(&rec, &dev, &cfg, &fs, 1));
}

static void test_telem_vin_deadband_exceeded(void)
{
    printf("--- test_telem_vin_deadband_exceeded ---\n");
    config_t cfg = make_test_config(true, true);
    device_cfg_t dev = make_test_dev();
    telem_filter_state_t fs;
    memset(&fs, 0, sizeof(fs));

    telemetry_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.valid_mask = TELEM_VALID_VIN;
    rec.vin_mV = 12000;
    pf_advance_telem_baseline(&rec, &fs, 0);

    /* VIN changes by exactly the deadband (100 mV) */
    rec.vin_mV = 12100;
    TEST_ASSERT_TRUE(pf_should_emit_telemetry(&rec, &dev, &cfg, &fs, 1));
}

static void test_telem_vin_within_deadband(void)
{
    printf("--- test_telem_vin_within_deadband ---\n");
    config_t cfg = make_test_config(true, true);
    device_cfg_t dev = make_test_dev();
    telem_filter_state_t fs;
    memset(&fs, 0, sizeof(fs));

    telemetry_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.valid_mask = TELEM_VALID_VIN;
    rec.vin_mV = 12000;
    pf_advance_telem_baseline(&rec, &fs, 0);

    /* VIN changes by 99 mV — below deadband (100) */
    rec.vin_mV = 12099;
    TEST_ASSERT_FALSE(pf_should_emit_telemetry(&rec, &dev, &cfg, &fs, 1));
}

static void test_telem_vout_deadband(void)
{
    printf("--- test_telem_vout_deadband ---\n");
    config_t cfg = make_test_config(true, true);
    device_cfg_t dev = make_test_dev();
    telem_filter_state_t fs;
    memset(&fs, 0, sizeof(fs));

    telemetry_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.valid_mask = TELEM_VALID_VOUT;
    rec.vout_mV = 1000;
    pf_advance_telem_baseline(&rec, &fs, 0);

    /* Change by 19 mV — below deadband (20) */
    rec.vout_mV = 1019;
    TEST_ASSERT_FALSE(pf_should_emit_telemetry(&rec, &dev, &cfg, &fs, 1));

    /* Change by exactly 20 mV */
    rec.vout_mV = 1020;
    TEST_ASSERT_TRUE(pf_should_emit_telemetry(&rec, &dev, &cfg, &fs, 1));
}

static void test_telem_heartbeat_triggers_emit(void)
{
    printf("--- test_telem_heartbeat_triggers_emit ---\n");
    config_t cfg = make_test_config(true, true);
    device_cfg_t dev = make_test_dev();
    telem_filter_state_t fs;
    memset(&fs, 0, sizeof(fs));

    telemetry_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.valid_mask = TELEM_VALID_VIN;
    rec.vin_mV = 12000;
    pf_advance_telem_baseline(&rec, &fs, 0);

    /* At tick 9999 — still within heartbeat (10000 ms) */
    TEST_ASSERT_FALSE(pf_should_emit_telemetry(&rec, &dev, &cfg, &fs, 9999));

    /* At tick 10000 — heartbeat expired */
    TEST_ASSERT_TRUE(pf_should_emit_telemetry(&rec, &dev, &cfg, &fs, 10000));
}

static void test_telem_valid_mask_change_triggers_emit(void)
{
    printf("--- test_telem_valid_mask_change_triggers_emit ---\n");
    config_t cfg = make_test_config(true, true);
    device_cfg_t dev = make_test_dev();
    telem_filter_state_t fs;
    memset(&fs, 0, sizeof(fs));

    telemetry_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.valid_mask = TELEM_VALID_VIN;
    rec.vin_mV = 12000;
    pf_advance_telem_baseline(&rec, &fs, 0);

    /* valid_mask changes (new field appeared) */
    rec.valid_mask = TELEM_VALID_VIN | TELEM_VALID_VOUT;
    TEST_ASSERT_TRUE(pf_should_emit_telemetry(&rec, &dev, &cfg, &fs, 1));
}

static void test_telem_per_device_override(void)
{
    printf("--- test_telem_per_device_override ---\n");
    config_t cfg = make_test_config(true, true);
    device_cfg_t dev = make_test_dev();
    dev.deadband_vin_mV = 500;  /* Override: 500 mV instead of global 100 */
    telem_filter_state_t fs;
    memset(&fs, 0, sizeof(fs));

    telemetry_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.valid_mask = TELEM_VALID_VIN;
    rec.vin_mV = 12000;
    pf_advance_telem_baseline(&rec, &fs, 0);

    /* Change by 200 mV — exceeds global (100) but NOT per-device (500) */
    rec.vin_mV = 12200;
    TEST_ASSERT_FALSE(pf_should_emit_telemetry(&rec, &dev, &cfg, &fs, 1));

    /* Change by 500 mV — meets per-device threshold */
    rec.vin_mV = 12500;
    TEST_ASSERT_TRUE(pf_should_emit_telemetry(&rec, &dev, &cfg, &fs, 1));
}

static void test_telem_negative_deadband(void)
{
    printf("--- test_telem_negative_deadband ---\n");
    config_t cfg = make_test_config(true, true);
    device_cfg_t dev = make_test_dev();
    telem_filter_state_t fs;
    memset(&fs, 0, sizeof(fs));

    telemetry_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.valid_mask = TELEM_VALID_VIN;
    rec.vin_mV = 12000;
    pf_advance_telem_baseline(&rec, &fs, 0);

    /* VIN drops by 100 mV — should trigger (absolute comparison) */
    rec.vin_mV = 11900;
    TEST_ASSERT_TRUE(pf_should_emit_telemetry(&rec, &dev, &cfg, &fs, 1));
}

/*******************************************************************************
 * Status filter tests
 ******************************************************************************/

static void test_status_first_sample_emitted_when_initial_true(void)
{
    printf("--- test_status_first_sample_emitted_when_initial_true ---\n");
    config_t cfg = make_test_config(true, true);
    device_cfg_t dev = make_test_dev();
    status_filter_state_t fs;
    memset(&fs, 0, sizeof(fs));
    fs.have_last = false;

    status_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.valid_mask = STATUS_VALID_ALL;

    TEST_ASSERT_TRUE(pf_should_emit_status(&rec, &dev, &cfg, &fs, 0));
}

static void test_status_first_sample_suppressed_when_initial_false(void)
{
    printf("--- test_status_first_sample_suppressed_when_initial_false ---\n");
    config_t cfg = make_test_config(true, true);
    cfg.reporting.status_emit_initial = false;
    device_cfg_t dev = make_test_dev();
    status_filter_state_t fs;
    memset(&fs, 0, sizeof(fs));
    fs.have_last = false;

    status_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.valid_mask = STATUS_VALID_ALL;

    /* First sample suppressed */
    TEST_ASSERT_FALSE(pf_should_emit_status(&rec, &dev, &cfg, &fs, 0));

    /* But after baseline is set, a changed sample emits */
    pf_advance_status_baseline(&rec, &fs, 0);
    rec.status_word = 0x0001;
    TEST_ASSERT_TRUE(pf_should_emit_status(&rec, &dev, &cfg, &fs, 1));
}

static void test_status_filter_disabled_always_emits(void)
{
    printf("--- test_status_filter_disabled_always_emits ---\n");
    config_t cfg = make_test_config(true, false);
    device_cfg_t dev = make_test_dev();
    status_filter_state_t fs;
    memset(&fs, 0, sizeof(fs));
    fs.have_last = true;
    fs.last_valid_mask = STATUS_VALID_ALL;

    status_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.valid_mask = STATUS_VALID_ALL;

    TEST_ASSERT_TRUE(pf_should_emit_status(&rec, &dev, &cfg, &fs, 1));
}

static void test_status_identical_suppressed(void)
{
    printf("--- test_status_identical_suppressed ---\n");
    config_t cfg = make_test_config(true, true);
    device_cfg_t dev = make_test_dev();
    status_filter_state_t fs;
    memset(&fs, 0, sizeof(fs));

    status_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.valid_mask = STATUS_VALID_ALL;
    rec.status_word = 0x0000;
    rec.status_vout = 0x00;
    rec.status_iout = 0x00;
    rec.status_temperature = 0x00;
    pf_advance_status_baseline(&rec, &fs, 0);

    /* Same values, tick=1 */
    TEST_ASSERT_FALSE(pf_should_emit_status(&rec, &dev, &cfg, &fs, 1));
}

static void test_status_word_change_triggers_emit(void)
{
    printf("--- test_status_word_change_triggers_emit ---\n");
    config_t cfg = make_test_config(true, true);
    device_cfg_t dev = make_test_dev();
    status_filter_state_t fs;
    memset(&fs, 0, sizeof(fs));

    status_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.valid_mask = STATUS_VALID_ALL;
    rec.status_word = 0x0000;
    pf_advance_status_baseline(&rec, &fs, 0);

    /* STATUS_WORD changes — any bit flip triggers emit */
    rec.status_word = 0x0001;
    TEST_ASSERT_TRUE(pf_should_emit_status(&rec, &dev, &cfg, &fs, 1));
}

static void test_status_vout_change_triggers_emit(void)
{
    printf("--- test_status_vout_change_triggers_emit ---\n");
    config_t cfg = make_test_config(true, true);
    device_cfg_t dev = make_test_dev();
    status_filter_state_t fs;
    memset(&fs, 0, sizeof(fs));

    status_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.valid_mask = STATUS_VALID_ALL;
    pf_advance_status_baseline(&rec, &fs, 0);

    rec.status_vout = 0x10;
    TEST_ASSERT_TRUE(pf_should_emit_status(&rec, &dev, &cfg, &fs, 1));
}

static void test_status_heartbeat_triggers_emit(void)
{
    printf("--- test_status_heartbeat_triggers_emit ---\n");
    config_t cfg = make_test_config(true, true);
    device_cfg_t dev = make_test_dev();
    status_filter_state_t fs;
    memset(&fs, 0, sizeof(fs));

    status_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.valid_mask = STATUS_VALID_ALL;
    pf_advance_status_baseline(&rec, &fs, 0);

    /* At tick 299999 — within heartbeat (300000 ms) */
    TEST_ASSERT_FALSE(pf_should_emit_status(&rec, &dev, &cfg, &fs, 299999));

    /* At tick 300000 — heartbeat expired */
    TEST_ASSERT_TRUE(pf_should_emit_status(&rec, &dev, &cfg, &fs, 300000));
}

static void test_status_heartbeat_fires_after_repeated_suppress(void)
{
    printf("--- test_status_heartbeat_fires_after_repeated_suppress ---\n");
    /* Regression test: when identical status samples are repeatedly suppressed,
     * the heartbeat must still fire once status_heartbeat_ms has elapsed since
     * the last *emitted* sample — baseline advance must NOT happen during
     * steady-state suppression, or last_emit_tick keeps resetting. */
    config_t cfg = make_test_config(true, true);
    cfg.reporting.status_heartbeat_ms = 1000;   /* 1 s heartbeat */
    device_cfg_t dev = make_test_dev();
    status_filter_state_t fs;
    memset(&fs, 0, sizeof(fs));

    status_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.valid_mask = STATUS_VALID_ALL;

    /* Emit and advance at tick 0 (initial sample) */
    TEST_ASSERT_TRUE(pf_should_emit_status(&rec, &dev, &cfg, &fs, 0));
    pf_advance_status_baseline(&rec, &fs, 0);

    /* Simulate 9 successive identical polls at 100-tick intervals (100..900).
     * All should be suppressed — no change, heartbeat not yet expired. */
    for (TickType_t t = 100; t <= 900; t += 100) {
        TEST_ASSERT_FALSE(pf_should_emit_status(&rec, &dev, &cfg, &fs, t));
        /* Critically: do NOT advance baseline here (as the fixed poll task does) */
    }

    /* At tick 1000 the heartbeat should fire */
    TEST_ASSERT_TRUE(pf_should_emit_status(&rec, &dev, &cfg, &fs, 1000));
}

static void test_status_heartbeat_disabled(void)
{
    printf("--- test_status_heartbeat_disabled ---\n");
    config_t cfg = make_test_config(true, true);
    cfg.reporting.status_heartbeat_ms = 0;  /* Disabled */
    device_cfg_t dev = make_test_dev();
    status_filter_state_t fs;
    memset(&fs, 0, sizeof(fs));

    status_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.valid_mask = STATUS_VALID_ALL;
    pf_advance_status_baseline(&rec, &fs, 0);

    /* Even at a very large tick, no heartbeat emit since disabled */
    TEST_ASSERT_FALSE(pf_should_emit_status(&rec, &dev, &cfg, &fs, 1000000));
}

static void test_status_valid_mask_change_triggers_emit(void)
{
    printf("--- test_status_valid_mask_change_triggers_emit ---\n");
    config_t cfg = make_test_config(true, true);
    device_cfg_t dev = make_test_dev();
    status_filter_state_t fs;
    memset(&fs, 0, sizeof(fs));

    status_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.valid_mask = STATUS_VALID_WORD;
    pf_advance_status_baseline(&rec, &fs, 0);

    rec.valid_mask = STATUS_VALID_WORD | STATUS_VALID_VOUT;
    TEST_ASSERT_TRUE(pf_should_emit_status(&rec, &dev, &cfg, &fs, 1));
}

/*******************************************************************************
 * Baseline advancement tests
 ******************************************************************************/

static void test_advance_telem_baseline_stores_values(void)
{
    printf("--- test_advance_telem_baseline_stores_values ---\n");
    telem_filter_state_t fs;
    memset(&fs, 0, sizeof(fs));

    telemetry_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.valid_mask = TELEM_VALID_ALL;
    rec.vin_mV = 12345;
    rec.vout_mV = 3300;
    rec.iin_mA = 100;
    rec.iout_mA = 200;
    rec.temp1_mC = 40000;
    rec.pout_mW = 660;

    pf_advance_telem_baseline(&rec, &fs, 42);

    TEST_ASSERT_TRUE(fs.have_last);
    TEST_ASSERT_MSG(fs.last_emit_tick == 42, "tick=%u", (unsigned)fs.last_emit_tick);
    TEST_ASSERT_MSG(fs.last_valid_mask == TELEM_VALID_ALL, "vm=0x%02X", fs.last_valid_mask);
    TEST_ASSERT_MSG(fs.last_vin_mV == 12345, "vin=%d", (int)fs.last_vin_mV);
    TEST_ASSERT_MSG(fs.last_vout_mV == 3300, "vout=%u", (unsigned)fs.last_vout_mV);
    TEST_ASSERT_MSG(fs.last_iin_mA == 100, "iin=%d", (int)fs.last_iin_mA);
    TEST_ASSERT_MSG(fs.last_iout_mA == 200, "iout=%d", (int)fs.last_iout_mA);
    TEST_ASSERT_MSG(fs.last_temp1_mC == 40000, "temp=%d", (int)fs.last_temp1_mC);
    TEST_ASSERT_MSG(fs.last_pout_mW == 660, "pout=%d", (int)fs.last_pout_mW);
}

static void test_advance_status_baseline_stores_values(void)
{
    printf("--- test_advance_status_baseline_stores_values ---\n");
    status_filter_state_t fs;
    memset(&fs, 0, sizeof(fs));

    status_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.valid_mask = STATUS_VALID_ALL;
    rec.status_word = 0xABCD;
    rec.status_vout = 0x12;
    rec.status_iout = 0x34;
    rec.status_temperature = 0x56;

    pf_advance_status_baseline(&rec, &fs, 99);

    TEST_ASSERT_TRUE(fs.have_last);
    TEST_ASSERT_MSG(fs.last_emit_tick == 99, "tick=%u", (unsigned)fs.last_emit_tick);
    TEST_ASSERT_MSG(fs.last_status_word == 0xABCD, "sw=0x%04X", fs.last_status_word);
    TEST_ASSERT_MSG(fs.last_status_vout == 0x12, "sv=0x%02X", fs.last_status_vout);
    TEST_ASSERT_MSG(fs.last_status_iout == 0x34, "si=0x%02X", fs.last_status_iout);
    TEST_ASSERT_MSG(fs.last_status_temp == 0x56, "st=0x%02X", fs.last_status_temp);
}

/*******************************************************************************
 * Main
 ******************************************************************************/
int main(void)
{
    printf("=== Publish Filter Unit Tests ===\n\n");

    /* Telemetry */
    test_telem_first_sample_always_emitted();
    test_telem_filter_disabled_always_emits();
    test_telem_identical_suppressed();
    test_telem_vin_deadband_exceeded();
    test_telem_vin_within_deadband();
    test_telem_vout_deadband();
    test_telem_heartbeat_triggers_emit();
    test_telem_valid_mask_change_triggers_emit();
    test_telem_per_device_override();
    test_telem_negative_deadband();

    /* Status */
    test_status_first_sample_emitted_when_initial_true();
    test_status_first_sample_suppressed_when_initial_false();
    test_status_filter_disabled_always_emits();
    test_status_identical_suppressed();
    test_status_word_change_triggers_emit();
    test_status_vout_change_triggers_emit();
    test_status_heartbeat_triggers_emit();
    test_status_heartbeat_fires_after_repeated_suppress();
    test_status_heartbeat_disabled();
    test_status_valid_mask_change_triggers_emit();

    /* Baseline */
    test_advance_telem_baseline_stores_values();
    test_advance_status_baseline_stores_values();

    printf("\n=== Results: %d passed, %d failed, %d total ===\n",
           tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}
