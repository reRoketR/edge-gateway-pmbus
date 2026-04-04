/*******************************************************************************
 * File Name:   buffer_flush.c
 *
 * Description: Buffer flush logic extracted from mqtt_gw_task.c for testability.
 *              Flash records are flushed first (oldest data), then RAM records.
 ******************************************************************************/

#include "buffer_flush.h"
#include "buffer_mgr.h"
#include "persistent_buffer.h"
#include "metrics.h"
#include "gateway_config.h"
#include "gateway_ipc.h"
#include <stdio.h>

uint16_t buffer_flush_records(buffer_publish_fn_t publish_fn)
{
    if (!g_config.buffer.enabled)
    {
        return 0u;
    }

    const bool flash_enabled = (g_config.buffer.flash_max_records > 0u);
    const uint32_t current_boot_gen = buffer_mgr_current_boot_gen();
    uint16_t flushed = 0u;
    buffer_record_t rec;

    /* Phase 1: Flush flash records first (oldest data) */
    if (flash_enabled)
    {
        while (flushed < g_config.buffer.flush_batch_size &&
               persistent_buffer_peek(&rec))
        {
            if (!publish_fn(rec.topic, rec.payload, rec.payload_len))
            {
                break;  /* Stop flushing on first failure */
            }
            uint32_t latency_us;
            if (buffer_record_same_boot_latency_us(&rec, current_boot_gen,
                                                   gateway_ipc_monotonic_ms(),
                                                   &latency_us))
            {
                metrics_record_read_to_publish_us(latency_us);
            }
            persistent_buffer_consume();
            flushed++;
        }
    }

    /* Phase 2: Flush RAM records (peek + consume = no FIFO breakage) */
    while (flushed < g_config.buffer.flush_batch_size &&
           buffer_mgr_peek(&rec))
    {
        if (!publish_fn(rec.topic, rec.payload, rec.payload_len))
        {
            break;  /* Stop flushing on first failure */
        }
        uint32_t latency_us;
        if (buffer_record_same_boot_latency_us(&rec, current_boot_gen,
                                               gateway_ipc_monotonic_ms(),
                                               &latency_us))
        {
            metrics_record_read_to_publish_us(latency_us);
        }
        buffer_mgr_consume();
        flushed++;
    }

    if (flushed > 0u)
    {
        printf("[MQTT] Flushed %u buffered records\n", (unsigned)flushed);
    }

    return flushed;
}
