/* Shared state and helper accessors for host-side QSPI buffer tests. */
#ifndef QSPI_MOCK_H
#define QSPI_MOCK_H

#include <stdint.h>

const uint8_t *qspi_mock_mmap_base(void);
void qspi_mock_reset(void);
void qspi_mock_corrupt_byte(uint32_t offset, uint8_t value);
void qspi_mock_corrupt_u32(uint32_t offset, uint32_t value);
void qspi_mock_fail_next_write(void);
void qspi_mock_fail_next_erase(void);
uint32_t qspi_mock_write_calls(void);
uint32_t qspi_mock_erase_calls(void);
uint32_t qspi_mock_metric_buffer_enqueued(void);
uint32_t qspi_mock_metric_buffer_dequeued(void);
uint32_t qspi_mock_metric_buffer_dropped(void);

#endif /* QSPI_MOCK_H */
