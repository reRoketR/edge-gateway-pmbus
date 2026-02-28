/*******************************************************************************
 * File Name:   test_buffer_ring.c
 *
 * Description: Host-side unit tests for the RAM ring buffer logic used by
 *              the buffer manager (buffer_mgr.c).
 *
 *              Since buffer_mgr.c uses FreeRTOS primitives, this test file
 *              re-implements the core ring buffer algorithm in a standalone
 *              form and verifies:
 *                - Basic put/get FIFO ordering
 *                - Buffer full detection
 *                - drop_oldest policy (overwrite oldest on overflow)
 *                - drop_newest policy (reject new on overflow)
 *                - Wrap-around correctness
 *                - Depth tracking
 *                - Payload truncation
 *
 * Build (host, MinGW/GCC):
 *   gcc -Wall -Wextra -Werror -o test_buffer_ring.exe tests/test_buffer_ring.c
 *
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/*******************************************************************************
 * Minimal ring buffer — extracted from buffer_mgr.c logic
 ******************************************************************************/

#define BUF_PAYLOAD_MAX  64u
#define BUF_TOPIC_MAX    32u

typedef struct {
    char     topic[BUF_TOPIC_MAX];
    char     payload[BUF_PAYLOAD_MAX];
    uint16_t payload_len;
} test_record_t;

typedef struct {
    test_record_t *ring;
    uint16_t capacity;
    uint16_t head;
    uint16_t tail;
    uint16_t count;
    bool     drop_oldest;
} test_ring_t;

static bool ring_init(test_ring_t *r, uint16_t cap, bool drop_oldest)
{
    r->ring = (test_record_t *)calloc(cap, sizeof(test_record_t));
    if (!r->ring) return false;
    r->capacity = cap;
    r->head = 0;
    r->tail = 0;
    r->count = 0;
    r->drop_oldest = drop_oldest;
    return true;
}

static void ring_free(test_ring_t *r)
{
    free(r->ring);
    r->ring = NULL;
}

/** @return true if accepted, false if dropped */
static bool ring_put(test_ring_t *r, const char *topic,
                     const char *payload, uint16_t payload_len)
{
    if (r->count >= r->capacity)
    {
        if (r->drop_oldest)
        {
            /* Overwrite oldest: advance tail */
            r->tail = (r->tail + 1u) % r->capacity;
            r->count--;
        }
        else
        {
            return false;  /* Drop new */
        }
    }

    test_record_t *rec = &r->ring[r->head];
    strncpy(rec->topic, topic, BUF_TOPIC_MAX - 1u);
    rec->topic[BUF_TOPIC_MAX - 1u] = '\0';

    uint16_t copy_len = payload_len;
    if (copy_len > BUF_PAYLOAD_MAX - 1u) copy_len = BUF_PAYLOAD_MAX - 1u;
    memcpy(rec->payload, payload, copy_len);
    rec->payload[copy_len] = '\0';
    rec->payload_len = copy_len;

    r->head = (r->head + 1u) % r->capacity;
    r->count++;
    return true;
}

static bool ring_get(test_ring_t *r, test_record_t *out)
{
    if (r->count == 0u) return false;

    memcpy(out, &r->ring[r->tail], sizeof(test_record_t));
    r->tail = (r->tail + 1u) % r->capacity;
    r->count--;
    return true;
}

/*******************************************************************************
 * Test framework (minimal)
 ******************************************************************************/
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT_TRUE(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL [%d]: %s\n", __LINE__, msg); \
        tests_failed++; \
    } else { \
        tests_passed++; \
    } \
} while(0)

#define ASSERT_EQ_INT(a, b, msg) do { \
    if ((a) != (b)) { \
        printf("  FAIL [%d]: %s (got %d, expected %d)\n", __LINE__, msg, (int)(a), (int)(b)); \
        tests_failed++; \
    } else { \
        tests_passed++; \
    } \
} while(0)

#define ASSERT_EQ_STR(a, b, msg) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("  FAIL [%d]: %s (got \"%s\", expected \"%s\")\n", __LINE__, msg, (a), (b)); \
        tests_failed++; \
    } else { \
        tests_passed++; \
    } \
} while(0)

/*******************************************************************************
 * Tests
 ******************************************************************************/

static void test_basic_put_get(void)
{
    printf("test_basic_put_get\n");
    test_ring_t r;
    ring_init(&r, 4, true);

    ASSERT_EQ_INT(r.count, 0, "initially empty");

    ring_put(&r, "topic/a", "payload_a", 9);
    ASSERT_EQ_INT(r.count, 1, "count after 1 put");

    ring_put(&r, "topic/b", "payload_b", 9);
    ASSERT_EQ_INT(r.count, 2, "count after 2 puts");

    test_record_t out;
    ASSERT_TRUE(ring_get(&r, &out), "get should succeed");
    ASSERT_EQ_STR(out.topic, "topic/a", "FIFO: first out = first in");
    ASSERT_EQ_STR(out.payload, "payload_a", "payload matches");
    ASSERT_EQ_INT(out.payload_len, 9, "payload_len matches");
    ASSERT_EQ_INT(r.count, 1, "count after 1 get");

    ASSERT_TRUE(ring_get(&r, &out), "get should succeed");
    ASSERT_EQ_STR(out.topic, "topic/b", "FIFO: second out = second in");
    ASSERT_EQ_INT(r.count, 0, "count after 2 gets");

    ASSERT_TRUE(!ring_get(&r, &out), "get on empty should fail");

    ring_free(&r);
}

static void test_fill_exact(void)
{
    printf("test_fill_exact\n");
    test_ring_t r;
    ring_init(&r, 3, true);

    ring_put(&r, "t0", "p0", 2);
    ring_put(&r, "t1", "p1", 2);
    ring_put(&r, "t2", "p2", 2);
    ASSERT_EQ_INT(r.count, 3, "full at capacity");

    test_record_t out;
    ring_get(&r, &out);
    ASSERT_EQ_STR(out.payload, "p0", "first out");
    ring_get(&r, &out);
    ASSERT_EQ_STR(out.payload, "p1", "second out");
    ring_get(&r, &out);
    ASSERT_EQ_STR(out.payload, "p2", "third out");
    ASSERT_EQ_INT(r.count, 0, "empty after drain");

    ring_free(&r);
}

static void test_drop_oldest_overflow(void)
{
    printf("test_drop_oldest_overflow\n");
    test_ring_t r;
    ring_init(&r, 3, true);  /* drop_oldest = true */

    ring_put(&r, "t0", "p0", 2);
    ring_put(&r, "t1", "p1", 2);
    ring_put(&r, "t2", "p2", 2);  /* full */

    /* Overflow: should drop p0 */
    ASSERT_TRUE(ring_put(&r, "t3", "p3", 2), "put on full w/ drop_oldest returns true");
    ASSERT_EQ_INT(r.count, 3, "count stays at capacity");

    test_record_t out;
    ring_get(&r, &out);
    ASSERT_EQ_STR(out.payload, "p1", "oldest (p0) was dropped, p1 is first");
    ring_get(&r, &out);
    ASSERT_EQ_STR(out.payload, "p2", "p2 second");
    ring_get(&r, &out);
    ASSERT_EQ_STR(out.payload, "p3", "p3 third");
    ASSERT_EQ_INT(r.count, 0, "empty");

    ring_free(&r);
}

static void test_drop_newest_overflow(void)
{
    printf("test_drop_newest_overflow\n");
    test_ring_t r;
    ring_init(&r, 3, false);  /* drop_oldest = false → drop new */

    ring_put(&r, "t0", "p0", 2);
    ring_put(&r, "t1", "p1", 2);
    ring_put(&r, "t2", "p2", 2);  /* full */

    ASSERT_TRUE(!ring_put(&r, "t3", "p3", 2), "put on full w/ drop_newest returns false");
    ASSERT_EQ_INT(r.count, 3, "count stays at capacity");

    test_record_t out;
    ring_get(&r, &out);
    ASSERT_EQ_STR(out.payload, "p0", "original oldest preserved");
    ring_get(&r, &out);
    ASSERT_EQ_STR(out.payload, "p1", "second preserved");
    ring_get(&r, &out);
    ASSERT_EQ_STR(out.payload, "p2", "third preserved, new was dropped");

    ring_free(&r);
}

static void test_wraparound(void)
{
    printf("test_wraparound\n");
    test_ring_t r;
    ring_init(&r, 3, true);

    /* Fill and drain multiple times to exercise wrap-around */
    for (int cycle = 0; cycle < 5; cycle++)
    {
        char p[8];
        for (int i = 0; i < 3; i++)
        {
            snprintf(p, sizeof(p), "c%d_p%d", cycle, i);
            ring_put(&r, "t", p, (uint16_t)strlen(p));
        }
        ASSERT_EQ_INT(r.count, 3, "full each cycle");

        test_record_t out;
        for (int i = 0; i < 3; i++)
        {
            snprintf(p, sizeof(p), "c%d_p%d", cycle, i);
            ring_get(&r, &out);
            ASSERT_EQ_STR(out.payload, p, "wrap-around FIFO correct");
        }
        ASSERT_EQ_INT(r.count, 0, "empty after drain");
    }

    ring_free(&r);
}

static void test_interleaved_put_get(void)
{
    printf("test_interleaved_put_get\n");
    test_ring_t r;
    ring_init(&r, 4, true);

    test_record_t out;

    ring_put(&r, "t", "a", 1);
    ring_put(&r, "t", "b", 1);
    ring_get(&r, &out);
    ASSERT_EQ_STR(out.payload, "a", "interleaved a");

    ring_put(&r, "t", "c", 1);
    ring_put(&r, "t", "d", 1);
    ring_put(&r, "t", "e", 1);  /* head wraps */
    ASSERT_EQ_INT(r.count, 4, "interleaved count 4");

    ring_get(&r, &out);
    ASSERT_EQ_STR(out.payload, "b", "interleaved b");
    ring_get(&r, &out);
    ASSERT_EQ_STR(out.payload, "c", "interleaved c");
    ring_get(&r, &out);
    ASSERT_EQ_STR(out.payload, "d", "interleaved d");
    ring_get(&r, &out);
    ASSERT_EQ_STR(out.payload, "e", "interleaved e");
    ASSERT_EQ_INT(r.count, 0, "empty");

    ring_free(&r);
}

static void test_payload_truncation(void)
{
    printf("test_payload_truncation\n");
    test_ring_t r;
    ring_init(&r, 2, true);

    /* Payload longer than BUF_PAYLOAD_MAX-1 should be truncated */
    char long_payload[128];
    memset(long_payload, 'X', sizeof(long_payload));
    long_payload[sizeof(long_payload) - 1] = '\0';

    ring_put(&r, "topic", long_payload, (uint16_t)(sizeof(long_payload) - 1));

    test_record_t out;
    ring_get(&r, &out);
    ASSERT_EQ_INT(out.payload_len, BUF_PAYLOAD_MAX - 1, "payload truncated to max");
    ASSERT_EQ_INT((int)strlen(out.payload), BUF_PAYLOAD_MAX - 1,
                  "string length matches truncated len");

    ring_free(&r);
}

static void test_topic_truncation(void)
{
    printf("test_topic_truncation\n");
    test_ring_t r;
    ring_init(&r, 2, true);

    char long_topic[128];
    memset(long_topic, 'T', sizeof(long_topic));
    long_topic[sizeof(long_topic) - 1] = '\0';

    ring_put(&r, long_topic, "p", 1);

    test_record_t out;
    ring_get(&r, &out);
    ASSERT_EQ_INT((int)strlen(out.topic), BUF_TOPIC_MAX - 1,
                  "topic truncated to max");

    ring_free(&r);
}

static void test_massive_overflow(void)
{
    printf("test_massive_overflow\n");
    test_ring_t r;
    ring_init(&r, 3, true);

    /* Push 100 items into a size-3 buffer */
    for (int i = 0; i < 100; i++)
    {
        char p[8];
        snprintf(p, sizeof(p), "%d", i);
        ring_put(&r, "t", p, (uint16_t)strlen(p));
    }

    ASSERT_EQ_INT(r.count, 3, "count capped at capacity");

    /* Should contain the last 3: 97, 98, 99 */
    test_record_t out;
    ring_get(&r, &out);
    ASSERT_EQ_STR(out.payload, "97", "massive overflow: oldest surviving = 97");
    ring_get(&r, &out);
    ASSERT_EQ_STR(out.payload, "98", "massive overflow: 98");
    ring_get(&r, &out);
    ASSERT_EQ_STR(out.payload, "99", "massive overflow: 99");

    ring_free(&r);
}

static void test_single_capacity(void)
{
    printf("test_single_capacity\n");
    test_ring_t r;
    ring_init(&r, 1, true);

    ring_put(&r, "t", "first", 5);
    ASSERT_EQ_INT(r.count, 1, "cap-1: count=1");

    ring_put(&r, "t", "second", 6);  /* overwrites first */
    ASSERT_EQ_INT(r.count, 1, "cap-1: still 1");

    test_record_t out;
    ring_get(&r, &out);
    ASSERT_EQ_STR(out.payload, "second", "cap-1: latest item preserved");
    ASSERT_EQ_INT(r.count, 0, "cap-1: empty");

    ring_free(&r);
}

/*******************************************************************************
 * Main
 ******************************************************************************/
int main(void)
{
    printf("=== Buffer Ring Buffer Tests ===\n\n");

    test_basic_put_get();
    test_fill_exact();
    test_drop_oldest_overflow();
    test_drop_newest_overflow();
    test_wraparound();
    test_interleaved_put_get();
    test_payload_truncation();
    test_topic_truncation();
    test_massive_overflow();
    test_single_capacity();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return (tests_failed > 0) ? 1 : 0;
}
