#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../source/gateway_config.h"
#include "../source/gateway_ipc.h"
#include "../source/events.h"
#include "../source/cmd_handler.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

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

#define TEST_ASSERT_EQ_STR(exp, act)                                          \
    TEST_ASSERT_MSG(strcmp((exp), (act)) == 0,                                \
                    "expected \"%s\", got \"%s\"",                            \
                    (exp), (act))

/* Test helpers from rtos_stubs.c */
extern void test_queue_reset_all(void);
extern uint32_t test_notify_count(void);
extern void test_reset_notify_count(void);

static const device_cfg_t k_test_devices[] = {
    {
        .addr_7bit = 0x58,
        .label = "psu_a",
        .poll_period_ms = 500,
        .status_period_ms = 10000,
    },
};

const char *g_profile_name = "test_cmd_pipeline";
const config_t g_config = {
    .gw_id = "test_gw",
    .i2c = {
        .bus = 0,
        .speed_hz = 100000,
        .transaction_timeout_ms = 20,
        .retries = 2,
        .bus_recovery = false,
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

/* Minimal link stubs for gateway_ipc.c */
void buffer_mgr_signal_spill_task(void) {}
bool emergency_status_ring_put(const status_record_t *rec)
{
    (void)rec;
    return false;
}
bool emergency_event_ring_put(const event_record_t *evt)
{
    (void)evt;
    return false;
}
bool persistent_seq_init(void) { return true; }
uint32_t persistent_seq_get(void) { return 0u; }
uint32_t persistent_seq_get_boot_count(void) { return 1u; }
void persistent_seq_checkpoint(uint32_t seq) { (void)seq; }
static uint32_t s_queue_drops = 0u;
void metrics_inc_queue_drops(void) { s_queue_drops++; }
const char *event_type_str(event_type_t type)
{
    switch (type)
    {
        case EVT_QUEUE_OVERFLOW: return "QUEUE_OVERFLOW";
        case EVT_CMD_SUBSCRIBE_FAIL: return "CMD_SUBSCRIBE_FAIL";
        default: return "EVENT";
    }
}

/* Mirrored command-path state from mqtt_gw_task.c */
static bool           s_cmd_pending_valid = false;
static cmd_response_t s_cmd_pending_resp;
static bool           s_cmd_subscribed = false;
static bool           s_publish_should_succeed = true;
static cmd_response_t s_published[16];
static uint32_t       s_publish_count = 0u;

static cmd_raw_t make_raw(const char *json)
{
    cmd_raw_t raw;
    size_t len = strlen(json);

    memset(&raw, 0, sizeof(raw));
    if (len >= sizeof(raw.payload))
    {
        len = sizeof(raw.payload) - 1u;
    }
    memcpy(raw.payload, json, len);
    raw.payload[len] = '\0';
    raw.payload_len = (uint16_t)len;

    return raw;
}

static bool publish_cmd_response(const cmd_response_t *resp)
{
    if (!s_publish_should_succeed)
    {
        return false;
    }

    if (s_publish_count < (sizeof(s_published) / sizeof(s_published[0])))
    {
        s_published[s_publish_count] = *resp;
    }
    s_publish_count++;
    return true;
}

static void process_cmd_response_queue_mirror(void)
{
    if (s_cmd_pending_valid)
    {
        if (publish_cmd_response(&s_cmd_pending_resp))
        {
            cmd_inflight_remove(s_cmd_pending_resp.id);
            cmd_cache_put(&s_cmd_pending_resp);
            s_cmd_pending_valid = false;
        }
        else
        {
            return;
        }
    }

    cmd_response_t resp;
    while (xQueueReceive(gateway_ipc_cmd_response_queue(), &resp, 0) == pdTRUE)
    {
        if (publish_cmd_response(&resp))
        {
            cmd_inflight_remove(resp.id);
            cmd_cache_put(&resp);
        }
        else
        {
            s_cmd_pending_resp = resp;
            s_cmd_pending_valid = true;
            return;
        }
    }
}

static void process_cmd_raw_queue_mirror(void)
{
    cmd_raw_t raw;

    while (xQueueReceive(gateway_ipc_cmd_raw_queue(), &raw, 0) == pdTRUE)
    {
        cmd_request_t req;
        char id[CMD_ID_MAX];
        cmd_parse_result_t pr = cmd_handler_parse(&raw, &req, id);

        if (pr == CMD_PARSE_BAD_JSON)
        {
            continue;
        }

        if (pr == CMD_PARSE_BAD_JSON_WITH_ID ||
            pr == CMD_PARSE_BAD_REQUEST ||
            pr == CMD_PARSE_UNSUPPORTED)
        {
            cmd_status_t st = CMD_STATUS_BAD_REQUEST;
            if (pr == CMD_PARSE_BAD_JSON_WITH_ID)
            {
                st = CMD_STATUS_BAD_JSON;
            }
            else if (pr == CMD_PARSE_UNSUPPORTED)
            {
                st = CMD_STATUS_UNSUPPORTED;
            }

            cmd_response_t err_resp;
            cmd_handler_build_error(&err_resp, id, req.addr_7bit, st);

            if (publish_cmd_response(&err_resp))
            {
                cmd_cache_put(&err_resp);
            }
            else
            {
                s_cmd_pending_resp = err_resp;
                s_cmd_pending_valid = true;
                return;
            }
            continue;
        }

        cmd_response_t cached;
        if (cmd_cache_lookup(req.id, &cached))
        {
            (void)publish_cmd_response(&cached);
            continue;
        }

        if (cmd_inflight_check(req.id))
        {
            continue;
        }

        if (!cmd_inflight_add(req.id))
        {
            cmd_response_t busy_resp;
            cmd_handler_build_error(&busy_resp, req.id, req.addr_7bit,
                                    CMD_STATUS_QUEUE_FULL);
            if (publish_cmd_response(&busy_resp))
            {
                cmd_cache_put(&busy_resp);
            }
            else
            {
                s_cmd_pending_resp = busy_resp;
                s_cmd_pending_valid = true;
                return;
            }
            continue;
        }

        if (xQueueSend(gateway_ipc_cmd_request_queue(), &req, 0) != pdTRUE)
        {
            cmd_inflight_remove(req.id);

            cmd_response_t full_resp;
            cmd_handler_build_error(&full_resp, req.id, req.addr_7bit,
                                    CMD_STATUS_QUEUE_FULL);
            if (publish_cmd_response(&full_resp))
            {
                cmd_cache_put(&full_resp);
            }
            else
            {
                s_cmd_pending_resp = full_resp;
                s_cmd_pending_valid = true;
                return;
            }
        }
    }
}

static void mqtt_iteration_mirror(void)
{
    process_cmd_response_queue_mirror();
    if (!s_cmd_pending_valid && s_cmd_subscribed)
    {
        process_cmd_raw_queue_mirror();
    }
}

static bool enqueue_raw_from_callback(const char *json)
{
    cmd_raw_t raw = make_raw(json);
    if (xQueueSend(gateway_ipc_cmd_raw_queue(), &raw, 0) != pdTRUE)
    {
        return false;
    }
    gateway_ipc_notify_mqtt_task();
    return true;
}

static bool poll_enqueue_response_mirror(const cmd_response_t *resp)
{
    if (xQueueSend(gateway_ipc_cmd_response_queue(), resp, 0) != pdTRUE)
    {
        cmd_inflight_remove(resp->id);
        return false;
    }

    gateway_ipc_notify_mqtt_task();
    return true;
}

static void reset_env(void)
{
    test_queue_reset_all();
    test_reset_notify_count();
    cmd_handler_init();
    TEST_ASSERT_TRUE(gateway_ipc_init());
    gateway_ipc_register_mqtt_task((TaskHandle_t)0x1);

    s_cmd_pending_valid = false;
    memset(&s_cmd_pending_resp, 0, sizeof(s_cmd_pending_resp));
    s_cmd_subscribed = true;
    s_publish_should_succeed = true;
    memset(s_published, 0, sizeof(s_published));
    s_publish_count = 0u;
    s_queue_drops = 0u;
}

static void test_raw_command_moves_to_request_queue(void)
{
    printf("--- test_raw_command_moves_to_request_queue ---\n");
    reset_env();

    TEST_ASSERT_TRUE(enqueue_raw_from_callback(
        "{\"id\":\"a1\",\"addr\":88,\"wr\":[136],\"rd_len\":2}"));
    TEST_ASSERT_EQ_U32(1u, test_notify_count());

    mqtt_iteration_mirror();

    TEST_ASSERT_EQ_U32(0u, uxQueueMessagesWaiting(gateway_ipc_cmd_raw_queue()));
    TEST_ASSERT_EQ_U32(1u, uxQueueMessagesWaiting(gateway_ipc_cmd_request_queue()));
    TEST_ASSERT_EQ_U32(0u, s_publish_count);
    TEST_ASSERT_TRUE(cmd_inflight_check("a1"));

    cmd_request_t req;
    TEST_ASSERT_TRUE(xQueueReceive(gateway_ipc_cmd_request_queue(), &req, 0) == pdTRUE);
    TEST_ASSERT_EQ_STR("a1", req.id);
    TEST_ASSERT_EQ_U32(0x58u, req.addr_7bit);
    TEST_ASSERT_EQ_U32(1u, req.write_len);
    TEST_ASSERT_EQ_U32(136u, req.write_data[0]);
    TEST_ASSERT_EQ_U32(2u, req.read_len);
}

static void test_duplicate_inflight_is_suppressed(void)
{
    printf("--- test_duplicate_inflight_is_suppressed ---\n");
    reset_env();

    TEST_ASSERT_TRUE(enqueue_raw_from_callback("{\"id\":\"dup1\",\"addr\":88,\"wr\":[136]}"));
    mqtt_iteration_mirror();
    TEST_ASSERT_EQ_U32(1u, uxQueueMessagesWaiting(gateway_ipc_cmd_request_queue()));

    TEST_ASSERT_TRUE(enqueue_raw_from_callback("{\"id\":\"dup1\",\"addr\":88,\"wr\":[136]}"));
    mqtt_iteration_mirror();

    TEST_ASSERT_EQ_U32(1u, uxQueueMessagesWaiting(gateway_ipc_cmd_request_queue()));
    TEST_ASSERT_EQ_U32(0u, s_publish_count);
}

static void test_cached_response_replayed_without_enqueue(void)
{
    printf("--- test_cached_response_replayed_without_enqueue ---\n");
    reset_env();

    cmd_response_t cached = {
        .id = "cache1",
        .addr_7bit = 88u,
        .status = PMBUS_OK,
        .read_data = { 0x12u, 0x34u },
        .read_len = 2u,
        .exec_ms = 7u,
    };
    cmd_cache_put(&cached);

    TEST_ASSERT_TRUE(enqueue_raw_from_callback(
        "{\"id\":\"cache1\",\"addr\":88,\"wr\":[136],\"rd_len\":2}"));
    mqtt_iteration_mirror();

    TEST_ASSERT_EQ_U32(0u, uxQueueMessagesWaiting(gateway_ipc_cmd_request_queue()));
    TEST_ASSERT_EQ_U32(1u, s_publish_count);
    TEST_ASSERT_EQ_STR("cache1", s_published[0].id);
    TEST_ASSERT_EQ_U32(PMBUS_OK, s_published[0].status);
}

static void test_request_queue_full_returns_queue_full(void)
{
    printf("--- test_request_queue_full_returns_queue_full ---\n");
    reset_env();

    cmd_request_t filler;
    memset(&filler, 0, sizeof(filler));
    for (uint32_t i = 0; i < CMD_QUEUE_DEPTH; i++)
    {
        TEST_ASSERT_TRUE(xQueueSend(gateway_ipc_cmd_request_queue(), &filler, 0) == pdTRUE);
    }

    TEST_ASSERT_TRUE(enqueue_raw_from_callback(
        "{\"id\":\"full1\",\"addr\":88,\"wr\":[136]}"));
    mqtt_iteration_mirror();

    TEST_ASSERT_EQ_U32(1u, s_publish_count);
    TEST_ASSERT_EQ_STR("full1", s_published[0].id);
    TEST_ASSERT_EQ_U32(CMD_STATUS_QUEUE_FULL, s_published[0].status);
    TEST_ASSERT_FALSE(cmd_inflight_check("full1"));
    TEST_ASSERT_EQ_U32(CMD_QUEUE_DEPTH,
                       uxQueueMessagesWaiting(gateway_ipc_cmd_request_queue()));
}

static void test_inflight_full_returns_queue_full(void)
{
    printf("--- test_inflight_full_returns_queue_full ---\n");
    reset_env();

    char id[CMD_ID_MAX];
    for (uint32_t i = 0; i < CMD_INFLIGHT_DEPTH; i++)
    {
        snprintf(id, sizeof(id), "i%lu", (unsigned long)i);
        TEST_ASSERT_TRUE(cmd_inflight_add(id));
    }

    TEST_ASSERT_TRUE(enqueue_raw_from_callback(
        "{\"id\":\"busy1\",\"addr\":88,\"wr\":[136]}"));
    mqtt_iteration_mirror();

    TEST_ASSERT_EQ_U32(1u, s_publish_count);
    TEST_ASSERT_EQ_STR("busy1", s_published[0].id);
    TEST_ASSERT_EQ_U32(CMD_STATUS_QUEUE_FULL, s_published[0].status);
    TEST_ASSERT_FALSE(cmd_inflight_check("busy1"));
    TEST_ASSERT_EQ_U32(0u, uxQueueMessagesWaiting(gateway_ipc_cmd_request_queue()));
}

static void test_response_queue_full_removes_inflight(void)
{
    printf("--- test_response_queue_full_removes_inflight ---\n");
    reset_env();

    cmd_response_t filler;
    memset(&filler, 0, sizeof(filler));
    for (uint32_t i = 0; i < CMD_QUEUE_DEPTH; i++)
    {
        TEST_ASSERT_TRUE(xQueueSend(gateway_ipc_cmd_response_queue(), &filler, 0) == pdTRUE);
    }

    TEST_ASSERT_TRUE(cmd_inflight_add("respdrop"));

    cmd_response_t resp;
    cmd_handler_build_error(&resp, "respdrop", 88u, CMD_STATUS_BAD_REQUEST);

    TEST_ASSERT_FALSE(poll_enqueue_response_mirror(&resp));
    TEST_ASSERT_FALSE(cmd_inflight_check("respdrop"));
    TEST_ASSERT_EQ_U32(0u, test_notify_count());
}

static void test_pending_response_blocks_raw_processing_until_cleared(void)
{
    printf("--- test_pending_response_blocks_raw_processing_until_cleared ---\n");
    reset_env();

    s_publish_should_succeed = false;
    TEST_ASSERT_TRUE(enqueue_raw_from_callback(
        "{\"id\":\"bad1\",\"addr\":\"oops\"}"));

    mqtt_iteration_mirror();
    TEST_ASSERT_TRUE(s_cmd_pending_valid);
    TEST_ASSERT_EQ_U32(0u, uxQueueMessagesWaiting(gateway_ipc_cmd_request_queue()));

    TEST_ASSERT_TRUE(enqueue_raw_from_callback(
        "{\"id\":\"later1\",\"addr\":88,\"wr\":[136]}"));

    mqtt_iteration_mirror();
    TEST_ASSERT_TRUE(s_cmd_pending_valid);
    TEST_ASSERT_EQ_U32(1u, uxQueueMessagesWaiting(gateway_ipc_cmd_raw_queue()));
    TEST_ASSERT_EQ_U32(0u, uxQueueMessagesWaiting(gateway_ipc_cmd_request_queue()));

    s_publish_should_succeed = true;
    mqtt_iteration_mirror();
    TEST_ASSERT_FALSE(s_cmd_pending_valid);
    TEST_ASSERT_EQ_U32(0u, uxQueueMessagesWaiting(gateway_ipc_cmd_raw_queue()));
    TEST_ASSERT_EQ_U32(1u, uxQueueMessagesWaiting(gateway_ipc_cmd_request_queue()));
}

static void test_pending_retried_before_fresh_responses(void)
{
    printf("--- test_pending_retried_before_fresh_responses ---\n");
    reset_env();

    TEST_ASSERT_TRUE(cmd_inflight_add("old1"));
    TEST_ASSERT_TRUE(cmd_inflight_add("new1"));

    cmd_handler_build_error(&s_cmd_pending_resp, "old1", 88u, CMD_STATUS_BAD_REQUEST);
    s_cmd_pending_valid = true;

    cmd_response_t fresh;
    cmd_handler_build_error(&fresh, "new1", 88u, CMD_STATUS_QUEUE_FULL);
    TEST_ASSERT_TRUE(xQueueSend(gateway_ipc_cmd_response_queue(), &fresh, 0) == pdTRUE);

    process_cmd_response_queue_mirror();

    TEST_ASSERT_FALSE(s_cmd_pending_valid);
    TEST_ASSERT_EQ_U32(2u, s_publish_count);
    TEST_ASSERT_EQ_STR("old1", s_published[0].id);
    TEST_ASSERT_EQ_STR("new1", s_published[1].id);
    TEST_ASSERT_FALSE(cmd_inflight_check("old1"));
    TEST_ASSERT_FALSE(cmd_inflight_check("new1"));

    {
        cmd_response_t cached;
        TEST_ASSERT_TRUE(cmd_cache_lookup("old1", &cached));
        TEST_ASSERT_TRUE(cmd_cache_lookup("new1", &cached));
    }
}

int main(void)
{
    printf("=== test_cmd_pipeline ===\n\n");

    test_raw_command_moves_to_request_queue();
    test_duplicate_inflight_is_suppressed();
    test_cached_response_replayed_without_enqueue();
    test_request_queue_full_returns_queue_full();
    test_inflight_full_returns_queue_full();
    test_response_queue_full_removes_inflight();
    test_pending_response_blocks_raw_processing_until_cleared();
    test_pending_retried_before_fresh_responses();

    printf("\n=== Results: %d passed, %d failed, %d total ===\n",
           tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}
