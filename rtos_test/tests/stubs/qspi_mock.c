/* Host-side QSPI flash emulation for qspi_buffer tests. */

#include "qspi_mock.h"
#include "qspi_buffer.h"
#include "qspi_flash.h"
#include "metrics.h"
#include "cy_serial_flash_qspi.h"

#include <string.h>

#define QSPI_MOCK_PAGE_SIZE 256u

static uint8_t s_flash[QSPI_BUF_REGION_SIZE];
static uint32_t s_write_calls;
static uint32_t s_erase_calls;
static uint32_t s_metric_buffer_enqueued;
static uint32_t s_metric_buffer_dequeued;
static uint32_t s_metric_buffer_dropped;
static int s_fail_next_write;
static int s_fail_next_erase;

static int qspi_mock_range_valid(uint32_t addr, size_t length)
{
    return (addr < QSPI_BUF_REGION_SIZE) &&
           (length <= QSPI_BUF_REGION_SIZE) &&
           (addr <= (QSPI_BUF_REGION_SIZE - length));
}

const uint8_t *qspi_mock_mmap_base(void)
{
    return s_flash;
}

void qspi_mock_reset(void)
{
    memset(s_flash, 0xFF, sizeof(s_flash));
    s_write_calls = 0u;
    s_erase_calls = 0u;
    s_metric_buffer_enqueued = 0u;
    s_metric_buffer_dequeued = 0u;
    s_metric_buffer_dropped = 0u;
    s_fail_next_write = 0;
    s_fail_next_erase = 0;
}

void qspi_mock_corrupt_byte(uint32_t offset, uint8_t value)
{
    if (offset < sizeof(s_flash))
    {
        s_flash[offset] = value;
    }
}

void qspi_mock_corrupt_u32(uint32_t offset, uint32_t value)
{
    if (offset <= (sizeof(s_flash) - sizeof(value)))
    {
        memcpy(&s_flash[offset], &value, sizeof(value));
    }
}

void qspi_mock_fail_next_write(void)
{
    s_fail_next_write = 1;
}

void qspi_mock_fail_next_erase(void)
{
    s_fail_next_erase = 1;
}

uint32_t qspi_mock_write_calls(void)
{
    return s_write_calls;
}

uint32_t qspi_mock_erase_calls(void)
{
    return s_erase_calls;
}

uint32_t qspi_mock_metric_buffer_enqueued(void)
{
    return s_metric_buffer_enqueued;
}

uint32_t qspi_mock_metric_buffer_dequeued(void)
{
    return s_metric_buffer_dequeued;
}

uint32_t qspi_mock_metric_buffer_dropped(void)
{
    return s_metric_buffer_dropped;
}

cy_rslt_t cy_serial_flash_qspi_erase(uint32_t addr, size_t length)
{
    if (s_fail_next_erase)
    {
        s_fail_next_erase = 0;
        return 1u;
    }

    if ((addr % QSPI_BUF_SECTOR_SIZE) != 0u || (length % QSPI_BUF_SECTOR_SIZE) != 0u)
    {
        return 2u;
    }

    if (!qspi_mock_range_valid(addr, length))
    {
        return 3u;
    }

    memset(&s_flash[addr], 0xFF, length);
    s_erase_calls++;
    return CY_RSLT_SUCCESS;
}

cy_rslt_t cy_serial_flash_qspi_read(uint32_t addr, size_t length, uint8_t *data)
{
    if (data == NULL || !qspi_mock_range_valid(addr, length))
    {
        return 4u;
    }

    memcpy(data, &s_flash[addr], length);
    return CY_RSLT_SUCCESS;
}

cy_rslt_t cy_serial_flash_qspi_write(uint32_t addr, size_t length, const uint8_t *data)
{
    size_t pos = 0u;

    if (s_fail_next_write)
    {
        s_fail_next_write = 0;
        return 5u;
    }

    if (data == NULL || !qspi_mock_range_valid(addr, length))
    {
        return 6u;
    }

    while (pos < length)
    {
        size_t page_off = (size_t)((addr + pos) % QSPI_MOCK_PAGE_SIZE);
        size_t page_rem = QSPI_MOCK_PAGE_SIZE - page_off;
        size_t chunk = length - pos;
        if (chunk > page_rem)
        {
            chunk = page_rem;
        }

        for (size_t i = 0; i < chunk; i++)
        {
            uint8_t cur = s_flash[addr + pos + i];
            uint8_t next = data[pos + i];
            if ((uint8_t)(cur & next) != next)
            {
                return 7u;
            }
        }

        for (size_t i = 0; i < chunk; i++)
        {
            s_flash[addr + pos + i] &= data[pos + i];
        }

        pos += chunk;
    }

    s_write_calls++;
    return CY_RSLT_SUCCESS;
}

size_t qspi_flash_get_size(void)
{
    return QSPI_BUF_REGION_SIZE;
}

size_t qspi_flash_get_erase_size(void)
{
    return QSPI_BUF_SECTOR_SIZE;
}

#ifndef INTEGRATION_TEST
void metrics_inc_buffer_enqueued(void)
{
    s_metric_buffer_enqueued++;
}

void metrics_inc_buffer_dequeued(void)
{
    s_metric_buffer_dequeued++;
}

void metrics_inc_buffer_dropped(void)
{
    s_metric_buffer_dropped++;
}
#endif /* INTEGRATION_TEST */
