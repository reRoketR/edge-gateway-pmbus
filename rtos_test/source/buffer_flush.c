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
#include <string.h>

/** JSON/topic scratch buffers used only while publishing buffered records. */
#define BUFFER_FLUSH_JSON_BUF_SIZE   512u
#define BUFFER_FLUSH_TOPIC_BUF_SIZE   80u

static char s_publish_json[BUFFER_FLUSH_JSON_BUF_SIZE];
static char s_publish_topic[BUFFER_FLUSH_TOPIC_BUF_SIZE];

static const char *lookup_device_label(uint8_t addr_7bit)
{
    if (g_config.devices == NULL)
    {
        return NULL;
    }

    for (uint8_t i = 0u; i < g_config.num_devices; i++)
    {
        if (g_config.devices[i].addr_7bit == addr_7bit)
        {
            return g_config.devices[i].label;
        }
    }

    return NULL;
}

static bool prepare_publish_record(const buffer_record_t *rec,
                                   char *topic, size_t topic_sz,
                                   char *payload, size_t payload_sz)
{
    if (rec == NULL || topic == NULL || payload == NULL)
    {
        return false;
    }

    switch ((buffer_record_kind_t)rec->kind)
    {
        case BUFFER_RECORD_TELEMETRY:
        {
            telemetry_record_t telem;
            buffer_record_to_telemetry(rec, &telem,
                                       lookup_device_label(rec->payload.telemetry.addr_7bit));
            if (encode_telemetry_json(&telem, payload, payload_sz) <= 0)
            {
                return false;
            }
            return (build_device_topic(topic, topic_sz, telem.addr_7bit,
                                       "telemetry") > 0);
        }

        case BUFFER_RECORD_STATUS:
        {
            status_record_t status;
            buffer_record_to_status(rec, &status,
                                    lookup_device_label(rec->payload.status.addr_7bit));
            if (encode_status_json(&status, payload, payload_sz) <= 0)
            {
                return false;
            }
            return (build_device_topic(topic, topic_sz, status.addr_7bit,
                                       "status") > 0);
        }

        case BUFFER_RECORD_EVENT:
        {
            event_record_t evt;
            buffer_record_to_event(rec, &evt);
            if (encode_event_json(&evt, payload, payload_sz) <= 0)
            {
                return false;
            }
            return (build_events_topic(topic, topic_sz) > 0);
        }

        default:
            return false;
    }
}

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
        while (flushed < g_config.buffer.flush_batch_size)
        {
            persistent_buffer_lock();
            if (!persistent_buffer_peek(&rec))
            {
                persistent_buffer_unlock();
                break;
            }

            if (!prepare_publish_record(&rec, s_publish_topic,
                                        BUFFER_FLUSH_TOPIC_BUF_SIZE,
                                        s_publish_json,
                                        BUFFER_FLUSH_JSON_BUF_SIZE))
            {
                printf("[MQTT] WARN: invalid buffered flash record, dropping\n");
                (void)persistent_buffer_consume();
                persistent_buffer_unlock();
                metrics_inc_buffer_dropped();
                continue;
            }

            uint32_t publish_start_ms = gateway_ipc_monotonic_ms();
            if (!publish_fn(s_publish_topic, s_publish_json,
                            (size_t)strlen(s_publish_json)))
            {
                persistent_buffer_unlock();
                break;  /* Stop flushing on first failure */
            }
            uint32_t publish_done_ms = gateway_ipc_monotonic_ms();
            uint32_t latency_us;
            uint32_t before_publish_us;
            if (buffer_record_same_boot_latency_us(&rec, current_boot_gen,
                                                   publish_done_ms,
                                                   &latency_us) &&
                buffer_record_same_boot_latency_us(&rec, current_boot_gen,
                                                   publish_start_ms,
                                                   &before_publish_us))
            {
                metrics_record_telemetry_path_us(latency_us,
                                                 before_publish_us,
                                                 latency_us - before_publish_us);
            }
            persistent_buffer_consume();
            persistent_buffer_unlock();
            flushed++;
        }
    }

    /* Phase 2: Flush RAM records (peek + consume = no FIFO breakage) */
    while (flushed < g_config.buffer.flush_batch_size &&
           buffer_mgr_peek(&rec))
    {
        if (!prepare_publish_record(&rec, s_publish_topic,
                                    BUFFER_FLUSH_TOPIC_BUF_SIZE,
                                    s_publish_json,
                                    BUFFER_FLUSH_JSON_BUF_SIZE))
        {
            printf("[MQTT] WARN: invalid buffered RAM record, dropping\n");
            (void)buffer_mgr_consume();
            metrics_inc_buffer_dropped();
            continue;
        }

        uint32_t publish_start_ms = gateway_ipc_monotonic_ms();
        if (!publish_fn(s_publish_topic, s_publish_json,
                        (size_t)strlen(s_publish_json)))
        {
            break;  /* Stop flushing on first failure */
        }
        uint32_t publish_done_ms = gateway_ipc_monotonic_ms();
        uint32_t latency_us;
        uint32_t before_publish_us;
        if (buffer_record_same_boot_latency_us(&rec, current_boot_gen,
                                               publish_done_ms,
                                               &latency_us) &&
            buffer_record_same_boot_latency_us(&rec, current_boot_gen,
                                               publish_start_ms,
                                               &before_publish_us))
        {
            metrics_record_telemetry_path_us(latency_us,
                                             before_publish_us,
                                             latency_us - before_publish_us);
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
