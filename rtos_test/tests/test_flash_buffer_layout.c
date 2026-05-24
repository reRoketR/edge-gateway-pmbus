/*******************************************************************************
 * File Name:   test_flash_buffer_layout.c
 *
 * Description: Focused host-side checks for the internal flash persistent
 *              record layout after adding buffered timing metadata.
 ******************************************************************************/

#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "../source/flash_buffer.h"

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
#define TEST_ASSERT_EQ_U32(exp, act)                                          \
    TEST_ASSERT_MSG((uint32_t)(exp) == (uint32_t)(act),                       \
                    "expected %lu, got %lu",                                  \
                    (unsigned long)(uint32_t)(exp),                           \
                    (unsigned long)(uint32_t)(act))

static void test_flash_layout_contract(void)
{
    printf("--- test_flash_layout_contract ---\n");

    TEST_ASSERT_EQ_U32(FLASH_BUF_ROW_SIZE, sizeof(flash_data_row_t));
    TEST_ASSERT_EQ_U32(FLASH_META_VERSION, 2u);
    TEST_ASSERT_EQ_U32(492u, FLASH_RECORD_MAX_PAYLOAD);
    TEST_ASSERT_EQ_U32(4u, offsetof(flash_data_row_t, payload_len));
    TEST_ASSERT_EQ_U32(6u, offsetof(flash_data_row_t, kind));
    TEST_ASSERT_EQ_U32(7u, offsetof(flash_data_row_t, reserved));
    TEST_ASSERT_EQ_U32(8u, offsetof(flash_data_row_t, origin_read_start_ms));
    TEST_ASSERT_EQ_U32(12u, offsetof(flash_data_row_t, origin_boot_gen));
    TEST_ASSERT_EQ_U32(16u, offsetof(flash_data_row_t, payload));
    TEST_ASSERT_EQ_U32(508u, offsetof(flash_data_row_t, crc32));
}

static void test_flash_row_binary_fields_roundtrip(void)
{
    printf("--- test_flash_row_binary_fields_roundtrip ---\n");

    flash_data_row_t row;
    memset(&row, 0xFF, sizeof(row));

    row.magic = FLASH_RECORD_MAGIC;
    row.payload_len = 5u;
    row.kind = BUFFER_RECORD_EVENT;
    row.origin_read_start_ms = 4321u;
    row.origin_boot_gen = 77u;
    memcpy(row.payload, "alpha", 5u);
    row.crc32 = 0x11223344u;

    TEST_ASSERT_EQ_U32(4321u, row.origin_read_start_ms);
    TEST_ASSERT_EQ_U32(77u, row.origin_boot_gen);
    TEST_ASSERT_TRUE(memcmp(row.payload, "alpha", 5u) == 0);
    TEST_ASSERT_EQ_U32(BUFFER_RECORD_EVENT, row.kind);
}

int main(void)
{
    printf("=== Flash Buffer Layout Tests ===\n\n");

    test_flash_layout_contract();
    test_flash_row_binary_fields_roundtrip();

    printf("\n=== Results: %d passed, %d failed, %d total ===\n",
           tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}
