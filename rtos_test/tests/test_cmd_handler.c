#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../source/cmd_handler.h"
#include "../source/gateway_config.h"

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

static cmd_parse_result_t parse_json(const char *json,
                                     cmd_request_t *req,
                                     char id_out[CMD_ID_MAX])
{
    cmd_raw_t raw = make_raw(json);
    return cmd_handler_parse(&raw, req, id_out);
}

static void test_parse_valid_shapes(void)
{
    printf("--- test_parse_valid_shapes ---\n");
    cmd_handler_init();

    cmd_request_t req;
    char id[CMD_ID_MAX];

    cmd_raw_t write_only = make_raw(
        "{\"id\":\"w1\",\"addr\":88,\"wr\":[136,18]}");
    TEST_ASSERT_EQ_INT(CMD_PARSE_OK,
                       cmd_handler_parse(&write_only, &req, id));
    TEST_ASSERT_EQ_STR("w1", req.id);
    TEST_ASSERT_EQ_STR("w1", id);
    TEST_ASSERT_EQ_U32(0x58u, req.addr_7bit);
    TEST_ASSERT_EQ_U32(2u, req.write_len);
    TEST_ASSERT_EQ_U32(136u, req.write_data[0]);
    TEST_ASSERT_EQ_U32(18u, req.write_data[1]);
    TEST_ASSERT_EQ_U32(0u, req.read_len);
    TEST_ASSERT_TRUE(req.pec);

    cmd_raw_t bare_read = make_raw(
        "{\"id\":\"r1\",\"addr\":88,\"rd_len\":2}");
    TEST_ASSERT_EQ_INT(CMD_PARSE_OK,
                       cmd_handler_parse(&bare_read, &req, id));
    TEST_ASSERT_EQ_STR("r1", req.id);
    TEST_ASSERT_EQ_U32(0u, req.write_len);
    TEST_ASSERT_EQ_U32(2u, req.read_len);
    TEST_ASSERT_TRUE(req.pec);

    cmd_raw_t wr_then_rd = make_raw(
        "{\"id\":\"rw1\",\"addr\":88,\"wr\":[136],\"rd_len\":2,\"pec\":false}");
    TEST_ASSERT_EQ_INT(CMD_PARSE_OK,
                       cmd_handler_parse(&wr_then_rd, &req, id));
    TEST_ASSERT_EQ_STR("rw1", req.id);
    TEST_ASSERT_EQ_U32(1u, req.write_len);
    TEST_ASSERT_EQ_U32(136u, req.write_data[0]);
    TEST_ASSERT_EQ_U32(2u, req.read_len);
    TEST_ASSERT_FALSE(req.pec);
}

static void test_parse_invalid_inputs(void)
{
    printf("--- test_parse_invalid_inputs ---\n");
    cmd_handler_init();

    cmd_request_t req;
    char id[CMD_ID_MAX];

    TEST_ASSERT_EQ_INT(CMD_PARSE_BAD_JSON,
                       parse_json("{\"addr\":88,\"wr\":[1]}", &req, id));

    TEST_ASSERT_EQ_INT(CMD_PARSE_BAD_REQUEST,
                       parse_json("{\"id\":\"bad_addr\",\"addr\":200}",
                                  &req, id));
    TEST_ASSERT_EQ_STR("bad_addr", id);

    TEST_ASSERT_EQ_INT(CMD_PARSE_BAD_REQUEST,
                       parse_json("{\"id\":\"bad_rd\",\"addr\":88,\"rd_len\":\"oops\"}",
                                  &req, id));
    TEST_ASSERT_EQ_STR("bad_rd", id);

    TEST_ASSERT_EQ_INT(CMD_PARSE_BAD_REQUEST,
                       parse_json("{\"id\":\"bad_pec\",\"addr\":88,\"rd_len\":1,\"pec\":\"true\"}",
                                  &req, id));
    TEST_ASSERT_EQ_STR("bad_pec", id);

    TEST_ASSERT_EQ_INT(CMD_PARSE_BAD_REQUEST,
                       parse_json("{\"id\":\"bad_wr\",\"addr\":88,\"wr\":{}}",
                                  &req, id));
    TEST_ASSERT_EQ_STR("bad_wr", id);

    {
        char json[320];
        strcpy(json, "{\"id\":\"oversized\",\"addr\":88,\"wr\":[");
        for (int i = 0; i < 33; i++)
        {
            char tmp[8];
            snprintf(tmp, sizeof(tmp), "%s1", (i > 0) ? "," : "");
            strcat(json, tmp);
        }
        strcat(json, "]}");

        TEST_ASSERT_EQ_INT(CMD_PARSE_BAD_REQUEST,
                           parse_json(json, &req, id));
        TEST_ASSERT_EQ_STR("oversized", id);
    }

    TEST_ASSERT_EQ_INT(CMD_PARSE_UNSUPPORTED,
                       parse_json("{\"id\":\"zero\",\"addr\":88}", &req, id));
    TEST_ASSERT_EQ_STR("zero", id);

    TEST_ASSERT_EQ_INT(CMD_PARSE_BAD_JSON_WITH_ID,
                       parse_json("{\"id\":\"recover\",\"addr\":88", &req, id));
    TEST_ASSERT_EQ_STR("recover", id);

    TEST_ASSERT_EQ_INT(CMD_PARSE_BAD_JSON,
                       parse_json("{\"addr\":88", &req, id));
    TEST_ASSERT_EQ_STR("", id);
}

static void test_order_independent_bad_request_classification(void)
{
    printf("--- test_order_independent_bad_request_classification ---\n");
    cmd_handler_init();

    cmd_request_t req;
    char id[CMD_ID_MAX];

    TEST_ASSERT_EQ_INT(CMD_PARSE_BAD_REQUEST,
                       parse_json("{\"id\":\"ord1\",\"addr\":88,\"rd_len\":\"x\"}",
                                  &req, id));
    TEST_ASSERT_EQ_STR("ord1", id);

    TEST_ASSERT_EQ_INT(CMD_PARSE_BAD_REQUEST,
                       parse_json("{\"rd_len\":\"x\",\"addr\":88,\"id\":\"ord2\"}",
                                  &req, id));
    TEST_ASSERT_EQ_STR("ord2", id);
}

static void test_encode_response(void)
{
    printf("--- test_encode_response ---\n");
    char buf[256];
    int len;

    cmd_response_t ok = {
        .id = "ok1",
        .addr_7bit = 88u,
        .status = PMBUS_OK,
        .read_data = { 18u, 52u },
        .read_len = 2u,
        .exec_ms = 7u,
    };

    len = cmd_handler_encode_response(&ok, buf, sizeof(buf));
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQ_STR(
        "{\"id\":\"ok1\",\"addr\":88,\"status\":\"OK\",\"data\":[18,52],\"exec_ms\":7}",
        buf);

    cmd_response_t bad_req;
    cmd_handler_build_error(&bad_req, "bad1", 88u, CMD_STATUS_BAD_REQUEST);
    len = cmd_handler_encode_response(&bad_req, buf, sizeof(buf));
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQ_STR(
        "{\"id\":\"bad1\",\"addr\":88,\"status\":\"BAD_REQUEST\",\"exec_ms\":0}",
        buf);

    cmd_response_t full;
    cmd_handler_build_error(&full, "full1", 88u, CMD_STATUS_QUEUE_FULL);
    len = cmd_handler_encode_response(&full, buf, sizeof(buf));
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQ_STR(
        "{\"id\":\"full1\",\"addr\":88,\"status\":\"QUEUE_FULL\",\"exec_ms\":0}",
        buf);
}

static void test_cache_semantics(void)
{
    printf("--- test_cache_semantics ---\n");
    cmd_handler_init();

    cmd_response_t stored = {
        .id = "cache1",
        .addr_7bit = 88u,
        .status = PMBUS_OK,
        .read_data = { 1u, 2u },
        .read_len = 2u,
        .exec_ms = 9u,
    };
    cmd_response_t loaded;

    cmd_cache_put(&stored);

    TEST_ASSERT_TRUE(cmd_cache_lookup("cache1", &loaded));
    TEST_ASSERT_EQ_STR("cache1", loaded.id);
    TEST_ASSERT_EQ_U32(PMBUS_OK, loaded.status);
    TEST_ASSERT_EQ_U32(2u, loaded.read_len);
    TEST_ASSERT_EQ_U32(9u, loaded.exec_ms);

    TEST_ASSERT_FALSE(cmd_cache_lookup("missing", &loaded));
}

static void test_inflight_semantics(void)
{
    printf("--- test_inflight_semantics ---\n");
    cmd_handler_init();

    char id[CMD_ID_MAX];
    for (uint32_t i = 0; i < CMD_INFLIGHT_DEPTH; i++)
    {
        snprintf(id, sizeof(id), "id%02lu", (unsigned long)i);
        TEST_ASSERT_TRUE(cmd_inflight_add(id));
        TEST_ASSERT_TRUE(cmd_inflight_check(id));
    }

    TEST_ASSERT_FALSE(cmd_inflight_add("overflow"));
    TEST_ASSERT_FALSE(cmd_inflight_check("overflow"));

    TEST_ASSERT_TRUE(cmd_inflight_check("id04"));
    cmd_inflight_remove("id04");
    TEST_ASSERT_FALSE(cmd_inflight_check("id04"));
    TEST_ASSERT_TRUE(cmd_inflight_check("id00"));
    TEST_ASSERT_TRUE(cmd_inflight_check("id08"));

    TEST_ASSERT_TRUE(cmd_inflight_add("newslot"));
    TEST_ASSERT_TRUE(cmd_inflight_check("newslot"));
}

int main(void)
{
    printf("=== test_cmd_handler ===\n\n");

    test_parse_valid_shapes();
    test_parse_invalid_inputs();
    test_order_independent_bad_request_classification();
    test_encode_response();
    test_cache_semantics();
    test_inflight_semantics();

    printf("\n=== Results: %d passed, %d failed, %d total ===\n",
           tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}
