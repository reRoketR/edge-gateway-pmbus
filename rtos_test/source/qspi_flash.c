/*******************************************************************************
 * File Name:   qspi_flash.c
 *
 * Description: QSPI external flash (S25FL512S) bring-up for D2a-1.
 *              Uses the Infineon serial-flash middleware and BSP-generated
 *              SMIF memory configuration (cycfg_qspi_memslot).
 *
 ******************************************************************************/
#include "qspi_flash.h"
#include "cy_serial_flash_qspi.h"
#include "cycfg_qspi_memslot.h"
#include "cybsp.h"

#include <stdio.h>
#include <string.h>

/*******************************************************************************
 * Constants
 ******************************************************************************/

/** QSPI clock frequency — conservative 25 MHz for reliable bring-up */
#define QSPI_FLASH_CLOCK_HZ    (25000000UL)

/** Size of the test pattern used in self-test */
#define SELF_TEST_PATTERN_LEN   256u

/*******************************************************************************
 * Module state
 ******************************************************************************/
static size_t s_flash_size  = 0;
static size_t s_sector_size = 0;

/*******************************************************************************
 * qspi_flash_init
 ******************************************************************************/
cy_rslt_t qspi_flash_init(void)
{
    cy_rslt_t result;

    printf("[QSPI] Initializing SMIF @ %lu Hz...\n",
           (unsigned long)QSPI_FLASH_CLOCK_HZ);

    result = cy_serial_flash_qspi_init(
        &S25FL512S_SlaveSlot_0,     /* BSP-generated memory config */
        CYBSP_QSPI_D0,
        CYBSP_QSPI_D1,
        CYBSP_QSPI_D2,
        CYBSP_QSPI_D3,
        NC,                         /* io4 — unused in Quad SPI */
        NC,                         /* io5 */
        NC,                         /* io6 */
        NC,                         /* io7 */
        CYBSP_QSPI_SCK,
        CYBSP_QSPI_SS,
        QSPI_FLASH_CLOCK_HZ);

    if (result != CY_RSLT_SUCCESS)
    {
        printf("[QSPI] ERROR: init failed (0x%08lX)\n", (unsigned long)result);
        return result;
    }

    s_flash_size  = cy_serial_flash_qspi_get_size();
    s_sector_size = cy_serial_flash_qspi_get_erase_size(0u);

    printf("[QSPI] S25FL512S detected, %lu MB, sector=%lu\n",
           (unsigned long)(s_flash_size / (1024u * 1024u)),
           (unsigned long)s_sector_size);

    return CY_RSLT_SUCCESS;
}

/*******************************************************************************
 * qspi_flash_self_test
 ******************************************************************************/
bool qspi_flash_self_test(void)
{
    if (s_flash_size == 0u || s_sector_size == 0u)
    {
        printf("[QSPI] Self-test: SKIP (not initialized)\n");
        return false;
    }

    /* Use the LAST sector to avoid collision with user data */
    uint32_t test_addr = (uint32_t)(s_flash_size - s_sector_size);

    uint8_t write_buf[SELF_TEST_PATTERN_LEN];
    uint8_t read_buf[SELF_TEST_PATTERN_LEN];

    /* Fill with a recognizable pattern */
    for (uint32_t i = 0; i < SELF_TEST_PATTERN_LEN; i++)
    {
        write_buf[i] = (uint8_t)((i & 1u) ? 0x55u : 0xAAu);
    }

    printf("[QSPI] Self-test: addr=0x%08lX, sector=%lu bytes\n",
           (unsigned long)test_addr, (unsigned long)s_sector_size);

    /* Step 1: Erase the test sector */
    printf("[QSPI] Self-test step 1/5: erasing...\n");
    cy_rslt_t res = cy_serial_flash_qspi_erase(test_addr, s_sector_size);
    if (res != CY_RSLT_SUCCESS)
    {
        printf("[QSPI] Self-test: FAIL (erase 0x%08lX)\n", (unsigned long)res);
        return false;
    }
    printf("[QSPI] Self-test step 1/5: erase OK\n");

    /* Step 2: Verify erased (all 0xFF) */
    printf("[QSPI] Self-test step 2/5: read-after-erase...\n");
    memset(read_buf, 0, sizeof(read_buf));
    res = cy_serial_flash_qspi_read(test_addr, SELF_TEST_PATTERN_LEN, read_buf);
    if (res != CY_RSLT_SUCCESS)
    {
        printf("[QSPI] Self-test: FAIL (read-after-erase 0x%08lX)\n",
               (unsigned long)res);
        return false;
    }
    for (uint32_t i = 0; i < SELF_TEST_PATTERN_LEN; i++)
    {
        if (read_buf[i] != 0xFFu)
        {
            printf("[QSPI] Self-test: FAIL (erase verify at offset %lu, got 0x%02X)\n",
                   (unsigned long)i, read_buf[i]);
            return false;
        }
    }
    printf("[QSPI] Self-test step 2/5: erase verify OK\n");

    /* Step 3: Write test pattern */
    printf("[QSPI] Self-test step 3/5: writing %u bytes...\n", SELF_TEST_PATTERN_LEN);
    res = cy_serial_flash_qspi_write(test_addr, SELF_TEST_PATTERN_LEN, write_buf);
    if (res != CY_RSLT_SUCCESS)
    {
        printf("[QSPI] Self-test: FAIL (write 0x%08lX)\n", (unsigned long)res);
        return false;
    }
    printf("[QSPI] Self-test step 3/5: write OK\n");

    /* Step 4: Read back and verify */
    printf("[QSPI] Self-test step 4/5: read-back verify...\n");
    memset(read_buf, 0, sizeof(read_buf));
    res = cy_serial_flash_qspi_read(test_addr, SELF_TEST_PATTERN_LEN, read_buf);
    if (res != CY_RSLT_SUCCESS)
    {
        printf("[QSPI] Self-test: FAIL (read-after-write 0x%08lX)\n",
               (unsigned long)res);
        return false;
    }
    if (memcmp(write_buf, read_buf, SELF_TEST_PATTERN_LEN) != 0)
    {
        printf("[QSPI] Self-test: FAIL (data mismatch)\n");
        return false;
    }
    printf("[QSPI] Self-test step 4/5: data match OK\n");

    /* Step 5: Final erase to leave the sector clean */
    printf("[QSPI] Self-test step 5/6: final erase...\n");
    res = cy_serial_flash_qspi_erase(test_addr, s_sector_size);
    if (res != CY_RSLT_SUCCESS)
    {
        printf("[QSPI] Self-test: FAIL (final erase 0x%08lX)\n",
               (unsigned long)res);
        return false;
    }
    printf("[QSPI] Self-test step 5/6: final erase OK\n");

    /* Step 6: Verify final erase */
    printf("[QSPI] Self-test step 6/6: final verify...\n");
    memset(read_buf, 0, sizeof(read_buf));
    res = cy_serial_flash_qspi_read(test_addr, SELF_TEST_PATTERN_LEN, read_buf);
    if (res != CY_RSLT_SUCCESS)
    {
        printf("[QSPI] Self-test: FAIL (read-after-final-erase 0x%08lX)\n",
               (unsigned long)res);
        return false;
    }
    for (uint32_t i = 0; i < SELF_TEST_PATTERN_LEN; i++)
    {
        if (read_buf[i] != 0xFFu)
        {
            printf("[QSPI] Self-test: FAIL (final erase verify at offset %lu, got 0x%02X)\n",
                   (unsigned long)i, read_buf[i]);
            return false;
        }
    }
    printf("[QSPI] Self-test step 6/6: final verify OK\n");

    printf("[QSPI] Self-test: PASS\n");
    return true;
}

/*******************************************************************************
 * Accessors
 ******************************************************************************/
size_t qspi_flash_get_size(void)
{
    return s_flash_size;
}

size_t qspi_flash_get_erase_size(void)
{
    return s_sector_size;
}
