/*******************************************************************************
 * File Name:   mqtt_gw_task.c
 *
 * Description: MQTT gateway task implementation (Task B per agent.md §6).
 *
 *              Lifecycle:
 *                1. Initialise Wi-Fi Connection Manager (WCM)
 *                2. Connect to Wi-Fi AP
 *                3. Initialise MQTT library, create MQTT instance
 *                4. Connect to MQTT broker
 *                5. Main loop: flush buffered records → publish metrics →
 *                   handle disconnect
 *
 *              Reconnect strategy:
 *                - Exponential backoff (backoff_min_ms … backoff_max_ms)
 *                - On Wi-Fi disconnect: reconnect Wi-Fi first
 *                - On MQTT disconnect: reconnect MQTT
 *
 *              The task never blocks on I2C or flash — only on queue receive
 *              and MQTT publish.
 *
 * Related Document: agent.md §6 (Task B), §4, §12
 *
 ******************************************************************************/

#include "mqtt_gw_task.h"

/* FreeRTOS */
#include "FreeRTOS.h"
#include "task.h"

/* Platform */
#include "cyhal.h"
#include "cybsp.h"
#include "cy_retarget_io.h"
#include "cy_wcm.h"

/* MQTT middleware */
#include "cy_mqtt_api.h"

/* LwIP for IP address printing */
#include "lwip/netif.h"

/* Project modules */
#include "gateway_config.h"
#include "gateway_ipc.h"
#include "mqtt_client_config.h"
#include "wifi_config.h"
#include "telemetry.h"
#include "events.h"
#include "metrics.h"
#include "wallclock.h"
#include "buffer_mgr.h"
#include "persistent_buffer.h"
#include "buffer_flush.h"
#include "cmd_handler.h"

#include <stdio.h>
#include <string.h>

/*******************************************************************************
 * Constants
 ******************************************************************************/

/** Max idle wait when no spill-task notification arrives (ms) */
#define MQTT_IDLE_WAIT_MAX_MS       1000u

/** Retry interval when metrics are due but telemetry/control path is busy (ms) */
#define METRICS_IDLE_RETRY_MS       250u

/** Max extra delay allowed before metrics are forced through while online (ms) */
#define METRICS_MAX_DEFERRAL_MS     5000u

/** JSON encoding buffer for metrics (keep in sync with test_json_encode.c) */
#define METRICS_JSON_BUF_SIZE       1408u

/** Topic string buffer */
#define TOPIC_BUF_SIZE              80u

/** MQTT handle descriptor */
#define MQTT_HANDLE_DESCRIPTOR      "MQTThandleID"

/** Detail strings for forced/offline MQTT transitions */
#define MQTT_DISCONNECT_DETAIL_CALLBACK       "broker_disconnect"
#define MQTT_DISCONNECT_DETAIL_PUBLISH_FAIL   "publish_fail"
#define MQTT_DISCONNECT_DETAIL_NOT_CONNECTED  "not_connected"

/*******************************************************************************
 * Private data
 ******************************************************************************/

/** MQTT connection handle */
cy_mqtt_t mqtt_connection;

/** Network buffer for MQTT library */
static uint8_t *s_mqtt_net_buf = NULL;

/** Reusable buffers (only used inside this task — no mutex needed) */
static char s_metrics_json_buf[METRICS_JSON_BUF_SIZE];
static char s_topic_buf[TOPIC_BUF_SIZE];

/** JSON buffer for command responses */
#define CMD_JSON_BUF_SIZE   384u
static char s_cmd_json_buf[CMD_JSON_BUF_SIZE];

/** Pending response slot: holds one response awaiting retry after publish failure */
static bool           s_cmd_pending_valid = false;
static cmd_response_t s_cmd_pending_resp;

/** True when the cmd/request topic is successfully subscribed */
static bool s_cmd_subscribed = false;

/** Subscribed cmd topic string (for callback topic matching) */
static char s_cmd_topic[TOPIC_BUF_SIZE];

/** Backoff state */
static uint32_t s_backoff_ms;

/** Metrics publish timer */
static TickType_t s_next_metrics_tick;

/** First tick when a due metrics publish started being deferred; 0 = not deferred */
static TickType_t s_metrics_due_since_tick;

/** Handle of this task, used for notification-based wakeups. */
static TaskHandle_t s_mqtt_task_handle = NULL;

/*******************************************************************************
 * Forward declarations
 ******************************************************************************/
static bool wifi_connect(void);
static bool mqtt_init_and_create(void);
static bool mqtt_broker_connect(void);
static void mqtt_event_cb(cy_mqtt_t handle, cy_mqtt_event_t event,
                          void *user_data);

static uint16_t flush_buffered_records(void);
static TickType_t mqtt_idle_wait_ticks(void);
static bool metrics_publish_due(TickType_t now);
static bool mqtt_can_publish_metrics_now(uint16_t flushed_this_loop);
static void publish_metrics_now(TickType_t now);
static void maybe_publish_metrics(uint16_t flushed_this_loop);
static uint8_t qos_for_topic(const char *topic);
static bool publish_json_qos(const char *topic, const char *payload,
                             size_t payload_len, uint8_t qos);
static bool mqtt_publish_failure_requires_offline(cy_rslt_t res, uint32_t consec_fails);
static void mqtt_request_disconnect(const char *detail);
static inline bool publish_telemetry_json(const char *topic, const char *payload,
                                          size_t payload_len)
{
    return publish_json_qos(topic, payload, payload_len,
                            g_config.mqtt.qos_telemetry);
}
static inline bool publish_control_json(const char *topic, const char *payload,
                                        size_t payload_len)
{
    return publish_json_qos(topic, payload, payload_len,
                            g_config.mqtt.qos_control);
}
static inline bool publish_buffered_json(const char *topic, const char *payload,
                                         size_t payload_len)
{
    return publish_json_qos(topic, payload, payload_len, qos_for_topic(topic));
}
static void backoff_reset(void);
static void backoff_wait(void);
static bool mqtt_subscribe_cmd_topic(void);
static void process_cmd_raw_queue(void);
static void process_cmd_response_queue(void);
static bool publish_cmd_response(const cmd_response_t *resp);

/** Volatile flag set from callback context, handled in main task loop. */
static volatile bool s_disconnect_pending = false;
/** Detail string for the next disconnect event posted from the main loop. */
static const char * volatile s_disconnect_detail =
    MQTT_DISCONNECT_DETAIL_CALLBACK;

/*******************************************************************************
 * Task entry point
 ******************************************************************************/
void mqtt_gw_task(void *pvParameters)
{
    (void)pvParameters;

    printf("[MQTT] Gateway MQTT task started\n");

    s_mqtt_task_handle = xTaskGetCurrentTaskHandle();
    buffer_mgr_register_flush_task(s_mqtt_task_handle);
    gateway_ipc_register_mqtt_task(s_mqtt_task_handle);
    cmd_handler_init();

    /* ---- Wi-Fi ---- */
    cy_wcm_config_t wcm_cfg = { .interface = CY_WCM_INTERFACE_TYPE_STA };
    if (CY_RSLT_SUCCESS != cy_wcm_init(&wcm_cfg))
    {
        printf("[MQTT] ERROR: cy_wcm_init failed\n");
        goto fail;
    }
    printf("[MQTT] WCM initialised\n");

    /* ---- MQTT library init (one-time) ---- */
    bool mqtt_lib_ready = false;

    /* ---- Connect phase: retry Wi-Fi + MQTT with backoff ---- */
    backoff_reset();
    for (;;)
    {

        /* Step 1: Wi-Fi */
        if (cy_wcm_is_connected_to_ap() == 0)
        {
            if (!wifi_connect())
            {
                printf("[MQTT] Wi-Fi connect failed, will retry...\n");
                backoff_wait();
                continue;
            }
        }


        /* Step 1b: SNTP wall-clock — start once after first Wi-Fi connect */
        wallclock_sntp_init();

        /* Step 2: MQTT library init (once) */
        if (!mqtt_lib_ready)
        {
            if (!mqtt_init_and_create())
            {
                printf("[MQTT] MQTT lib init failed, will retry...\n");
                backoff_wait();
                continue;
            }
            mqtt_lib_ready = true;
        }


        /* Step 3: Connect to broker */
        if (!mqtt_broker_connect())
        {
            printf("[MQTT] Broker connect failed, will retry...\n");
            backoff_wait();
            continue;
        }

        /* Connected! */
        gateway_ipc_set_mqtt_online(true);
        gateway_ipc_post_event(EVT_MQTT_CONNECTED, g_config.mqtt.host);
        if (!mqtt_subscribe_cmd_topic())
        {
            gateway_ipc_post_event(EVT_CMD_SUBSCRIBE_FAIL, "cmd_subscribe_fail");
        }
        backoff_reset();
        break;
    }

    /* Metrics timer */
    s_next_metrics_tick = xTaskGetTickCount() +
                          pdMS_TO_TICKS(g_config.metrics_period_ms);
    s_metrics_due_since_tick = 0u;

    /* ---- Main loop ---- */
    for (;;)
    {
        /* Check for disconnect signalled by callback */
        if (s_disconnect_pending)
        {
            const char *detail = s_disconnect_detail;
            s_disconnect_pending = false;
            s_disconnect_detail = MQTT_DISCONNECT_DETAIL_CALLBACK;

            if (gateway_ipc_is_mqtt_online())
            {
                if (strcmp(detail, MQTT_DISCONNECT_DETAIL_CALLBACK) == 0)
                {
                    printf("[MQTT] Disconnected (callback)\n");
                }
                else
                {
                    printf("[MQTT] Disconnected (%s)\n", detail);
                }

                gateway_ipc_set_mqtt_online(false);
                gateway_ipc_post_event(EVT_MQTT_DISCONNECTED, detail);
                s_cmd_subscribed = false;
            }
        }

        /* Check if we're still connected */
        if (!gateway_ipc_is_mqtt_online())
        {

            /* Attempt reconnect */
            printf("[MQTT] Connection lost \xe2\x80\x94 reconnecting...\n");

            /* Disconnect cleanly first (ignore errors) */
            (void)cy_mqtt_disconnect(mqtt_connection);

            /* Check Wi-Fi */
            if (cy_wcm_is_connected_to_ap() == 0)
            {
                printf("[MQTT] Wi-Fi down, reconnecting Wi-Fi...\n");
                if (!wifi_connect())
                {
                    backoff_wait();
                    continue;  /* retry from top of loop */
                }
            }


            /* Reconnect MQTT */
            if (mqtt_broker_connect())
            {
                gateway_ipc_set_mqtt_online(true);
                gateway_ipc_post_event(EVT_MQTT_CONNECTED, g_config.mqtt.host);
                if (!mqtt_subscribe_cmd_topic())
                {
                    gateway_ipc_post_event(EVT_CMD_SUBSCRIBE_FAIL, "cmd_subscribe_fail");
                }
                metrics_inc_mqtt_reconnects();
                backoff_reset();
            }
            else
            {
                backoff_wait();
                continue;  /* retry from top of loop */
            }
        }

        /* Flush buffered records (spill task feeds buffer_mgr,
         * this task is the sole publisher) */
        uint16_t flushed = flush_buffered_records();
        if (s_disconnect_pending) continue;

        /* Metrics (opportunistic: only when publish path is idle) */
        maybe_publish_metrics(flushed);

        /* --- Command path: responses then raw requests --- */
        process_cmd_response_queue();
        if (!s_cmd_pending_valid && s_cmd_subscribed)
        {
            process_cmd_raw_queue();
        }

        if (g_config.buffer.flush_batch_size > 0u &&
            flushed == g_config.buffer.flush_batch_size)
        {
            continue;
        }

        (void)ulTaskNotifyTake(pdTRUE, mqtt_idle_wait_ticks());
    }

fail:
    printf("[MQTT] FATAL: MQTT task cannot proceed. Halting.\n");
    for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
}

/*******************************************************************************
 * Wi-Fi connect (with retries)
 ******************************************************************************/
static bool wifi_connect(void)
{
    cy_wcm_connect_params_t conn_params;
    cy_wcm_ip_address_t ip;
    memset(&conn_params, 0, sizeof(conn_params));

    /* Bounds-check at compile time (CY_WCM_MAX_SSID_LEN=32, PASSPHRASE=63) */
    _Static_assert(sizeof(WIFI_SSID) <= sizeof(conn_params.ap_credentials.SSID),
                   "WIFI_SSID exceeds CY_WCM_MAX_SSID_LEN");
    _Static_assert(sizeof(WIFI_PASSWORD) <= sizeof(conn_params.ap_credentials.password),
                   "WIFI_PASSWORD exceeds CY_WCM_MAX_PASSPHRASE_LEN");

    strncpy((char *)conn_params.ap_credentials.SSID, WIFI_SSID,
            sizeof(conn_params.ap_credentials.SSID) - 1u);
    strncpy((char *)conn_params.ap_credentials.password, WIFI_PASSWORD,
            sizeof(conn_params.ap_credentials.password) - 1u);
    conn_params.ap_credentials.security = WIFI_SECURITY;

    printf("[MQTT] Wi-Fi connecting to '%s'...\n", WIFI_SSID);

    /* D1-1: Reduced from 3 to 1 inner retry so wifi_connect() blocks
     * at most ~5s before returning.  The outer task loop provides
     * additional retries with backoff and queue draining. */
    const uint32_t tries_per_attempt = 1u;

    for (uint32_t retry = 0; retry < tries_per_attempt; retry++)
    {
        cy_rslt_t res = cy_wcm_connect_ap(&conn_params, &ip);
        if (res == CY_RSLT_SUCCESS)
        {
            if (ip.version == CY_WCM_IP_VER_V4)
            {
                printf("[MQTT] Wi-Fi connected, IP=%s\n",
                       ip4addr_ntoa((const ip4_addr_t *)&ip.ip.v4));
            }
            else
            {
                printf("[MQTT] Wi-Fi connected (IPv6)\n");
            }

            return true;
        }

        printf("[MQTT] Wi-Fi failed (0x%lX), retry %lu/%lu\n",
               (unsigned long)res, (unsigned long)(retry + 1),
               (unsigned long)tries_per_attempt);
    }

    printf("[MQTT] Wi-Fi connect failed\n");
    return false;
}

/*******************************************************************************
 * MQTT init & create
 ******************************************************************************/
static bool mqtt_init_and_create(void)
{
    cy_rslt_t res;

    res = cy_mqtt_init();
    if (res != CY_RSLT_SUCCESS)
    {
        printf("[MQTT] ERROR: cy_mqtt_init failed (0x%lX)\n",
               (unsigned long)res);
        return false;
    }

    /* Allocate network buffer */
    s_mqtt_net_buf = (uint8_t *)pvPortMalloc(MQTT_NETWORK_BUFFER_SIZE);
    if (s_mqtt_net_buf == NULL)
    {
        printf("[MQTT] ERROR: network buffer allocation failed\n");
        return false;
    }

    /* Override broker address/port from the active profile (gateway_config).
     * g_config is the single source of truth for connection parameters. */
    broker_info.hostname     = g_config.mqtt.host;
    broker_info.hostname_len = (uint16_t)strlen(g_config.mqtt.host);
    broker_info.port         = (uint16_t)g_config.mqtt.port;

    /* Create MQTT instance */
    res = cy_mqtt_create(s_mqtt_net_buf, MQTT_NETWORK_BUFFER_SIZE,
                         security_info, &broker_info,
                         MQTT_HANDLE_DESCRIPTOR, &mqtt_connection);
    if (res != CY_RSLT_SUCCESS)
    {
        printf("[MQTT] ERROR: cy_mqtt_create failed (0x%lX)\n",
               (unsigned long)res);
        vPortFree(s_mqtt_net_buf);
        s_mqtt_net_buf = NULL;
        return false;
    }

    /* Register event callback */
    res = cy_mqtt_register_event_callback(mqtt_connection,
                                          mqtt_event_cb, NULL);
    if (res != CY_RSLT_SUCCESS)
    {
        printf("[MQTT] ERROR: register callback failed (0x%lX)\n",
               (unsigned long)res);
        (void)cy_mqtt_delete(mqtt_connection);
        vPortFree(s_mqtt_net_buf);
        s_mqtt_net_buf = NULL;
        return false;
    }

    printf("[MQTT] MQTT library initialised, instance created\n");
    return true;
}

/*******************************************************************************
 * MQTT broker connect
 ******************************************************************************/
static bool mqtt_broker_connect(void)
{
    printf("[MQTT] Connecting to broker %s:%u...\n",
           g_config.mqtt.host, (unsigned)g_config.mqtt.port);

    connection_info.client_id     = g_config.mqtt.client_id;
    connection_info.client_id_len = (uint16_t)strlen(g_config.mqtt.client_id);

    /* Build LWT topic and payload from g_config so they always match the
     * active profile (single source of truth). */
#if ENABLE_LWT_MESSAGE
    {
        char lwt_topic[96];
        char lwt_payload[128];
        snprintf(lwt_topic, sizeof(lwt_topic),
                 "%s/events", g_config.mqtt.base_topic);
        snprintf(lwt_payload, sizeof(lwt_payload),
                 "{\"type\":\"GATEWAY_UNEXPECTED_DISCONNECT\","
                 "\"detail\":\"lwt\",\"gw_id\":\"%s\"}",
                 g_config.gw_id);
        mqtt_config_set_lwt(lwt_topic, lwt_payload);
    }
#endif

    cy_rslt_t res = cy_mqtt_connect(mqtt_connection, &connection_info);
    if (res == CY_RSLT_SUCCESS)
    {
        printf("[MQTT] Connected to broker\n");
        return true;
    }

    printf("[MQTT] Broker connect failed (0x%lX)\n", (unsigned long)res);
    return false;
}

/*******************************************************************************
 * MQTT event callback (called from MQTT library context)
 ******************************************************************************/

static void mqtt_event_cb(cy_mqtt_t handle, cy_mqtt_event_t event,
                          void *user_data)
{
    (void)handle;
    (void)user_data;

    switch (event.type)
    {
        case CY_MQTT_EVENT_TYPE_DISCONNECT:
            /* Only touch volatile flag — no FreeRTOS API / printf here.
             * The main task loop will post the event and update IPC. */
            mqtt_request_disconnect(MQTT_DISCONNECT_DETAIL_CALLBACK);
            break;

        case CY_MQTT_EVENT_TYPE_SUBSCRIPTION_MESSAGE_RECEIVE:
        {
            /* Copy raw payload into cmd_raw_queue for processing
             * in the main task loop (no MQTT API calls allowed here). */
            cy_mqtt_received_msg_info_t *msg =
                &event.data.pub_msg.received_message;

            /* Only route messages from the cmd/request topic */
            if (msg->topic == NULL || msg->topic_len == 0 ||
                msg->topic_len != (uint16_t)strlen(s_cmd_topic) ||
                memcmp(msg->topic, s_cmd_topic, msg->topic_len) != 0)
            {
                break;  /* Not our topic — ignore */
            }

            if (msg->payload_len > 0 &&
                msg->payload_len <= CMD_RAW_PAYLOAD_MAX - 1u)
            {
                cmd_raw_t raw;
                memcpy(raw.payload, msg->payload,
                       msg->payload_len);
                raw.payload[msg->payload_len] = '\0';
                raw.payload_len = (uint16_t)msg->payload_len;

                if (xQueueSend(gateway_ipc_cmd_raw_queue(),
                               &raw, 0) == pdTRUE)
                {
                    gateway_ipc_notify_mqtt_task();
                }
                /* else: queue full — drop silently */
            }
            break;
        }

        default:
            break;
    }
}

/* process_telemetry_queue(), process_status_queue(), process_event_queue()
 * and drain_queues_to_buffer() removed — all upstream queue consumption is
 * now handled exclusively by the dedicated spill task (buffer_task in
 * buffer_mgr.c).  The MQTT task only reads from buffer_mgr.
 * See T-6 spill task architecture note. */

/*******************************************************************************
 * Buffer flush — all MQTT publishing happens here (single publisher)
 *
 * Uses peek/consume pattern to preserve FIFO ordering.
 * Flash records are flushed first (oldest data), then RAM records.
 ******************************************************************************/
static uint16_t flush_buffered_records(void)
{
    return buffer_flush_records(publish_buffered_json);
}

static TickType_t mqtt_idle_wait_ticks(void)
{
    const TickType_t max_wait_ticks = pdMS_TO_TICKS(MQTT_IDLE_WAIT_MAX_MS);
    TickType_t now = xTaskGetTickCount();

    if ((int32_t)(now - s_next_metrics_tick) >= 0)
    {
        return 0u;
    }

    TickType_t until_metrics = s_next_metrics_tick - now;
    return (until_metrics < max_wait_ticks) ? until_metrics : max_wait_ticks;
}

static bool metrics_publish_due(TickType_t now)
{
    return ((int32_t)(now - s_next_metrics_tick) >= 0);
}

static bool mqtt_can_publish_metrics_now(uint16_t flushed_this_loop)
{
    if (!gateway_ipc_is_mqtt_online() || s_disconnect_pending)
    {
        return false;
    }

    if (flushed_this_loop != 0u)
    {
        return false;
    }

    if (buffer_mgr_depth() != 0u)
    {
        return false;
    }

    if (g_config.buffer.flash_max_records > 0u &&
        persistent_buffer_depth() != 0u)
    {
        return false;
    }

    return true;
}

static void publish_metrics_now(TickType_t now)
{
    s_next_metrics_tick = now + pdMS_TO_TICKS(g_config.metrics_period_ms);
    s_metrics_due_since_tick = 0u;

    /* Update gauges before snapshot */
    metrics_set_buffer_depth_ram(buffer_mgr_depth());
    if (g_config.buffer.flash_max_records > 0u)
    {
        metrics_set_buffer_depth_flash(persistent_buffer_depth());
    }
    else
    {
        metrics_set_buffer_depth_flash(0u);
    }
    metrics_set_telemetry_queue_depth(gateway_ipc_telemetry_queue_depth());

    /* Try to get Wi-Fi RSSI */
    {
        cy_wcm_associated_ap_info_t ap_info;
        memset(&ap_info, 0, sizeof(ap_info));
        if (CY_RSLT_SUCCESS == cy_wcm_get_associated_ap_info(&ap_info))
        {
            metrics_set_wifi_rssi(ap_info.signal_strength);
        }
    }

    metrics_snapshot_t snap;
    metrics_snapshot_and_reset(&snap,
                               gateway_ipc_now_ms(),
                               gateway_ipc_monotonic_ms());

    int json_len = encode_metrics_json(&snap, s_metrics_json_buf, METRICS_JSON_BUF_SIZE);
    if (json_len <= 0)
    {
        printf("[MQTT] WARN: metrics JSON encode failed\n");
        return;
    }

    int topic_len = build_metrics_topic(s_topic_buf, TOPIC_BUF_SIZE);
    if (topic_len <= 0) return;

    if (!publish_json_qos(s_topic_buf, s_metrics_json_buf, (size_t)json_len,
                          g_config.mqtt.qos_metrics))
    {
        /* Don't buffer metrics — they're stale if delayed */
        printf("[MQTT] WARN: metrics publish failed (not buffered)\n");
    }
}

/*******************************************************************************
 * Metrics publishing
 ******************************************************************************/
static void maybe_publish_metrics(uint16_t flushed_this_loop)
{
    TickType_t now = xTaskGetTickCount();

    if (!metrics_publish_due(now))
    {
        return;
    }

    if (mqtt_can_publish_metrics_now(flushed_this_loop))
    {
        publish_metrics_now(now);
        return;
    }

    if (s_metrics_due_since_tick == 0u)
    {
        s_metrics_due_since_tick = now;
    }

    if (gateway_ipc_is_mqtt_online() &&
        !s_disconnect_pending &&
        ((int32_t)(now - s_metrics_due_since_tick) >=
         (int32_t)pdMS_TO_TICKS(METRICS_MAX_DEFERRAL_MS)))
    {
        publish_metrics_now(now);
        return;
    }

    s_next_metrics_tick = now + pdMS_TO_TICKS(METRICS_IDLE_RETRY_MS);
}

/*******************************************************************************
 * Publish helper
 ******************************************************************************/
static bool topic_has_suffix(const char *topic, const char *suffix)
{
    size_t topic_len = strlen(topic);
    size_t suffix_len = strlen(suffix);

    if (topic_len < suffix_len)
    {
        return false;
    }

    return strcmp(topic + topic_len - suffix_len, suffix) == 0;
}

static uint8_t qos_for_topic(const char *topic)
{
    if (topic_has_suffix(topic, "/telemetry"))
    {
        return g_config.mqtt.qos_telemetry;
    }

    if (topic_has_suffix(topic, "/metrics"))
    {
        return g_config.mqtt.qos_metrics;
    }

    return g_config.mqtt.qos_control;
}

static uint32_t s_consec_pub_fails = 0;

static bool publish_json_qos(const char *topic, const char *payload,
                             size_t payload_len, uint8_t qos)
{
    cy_mqtt_publish_info_t pub = {
        .qos         = (cy_mqtt_qos_t)qos,
        .retain      = false,
        .dup         = false,
        .topic       = topic,
        .topic_len   = (uint16_t)strlen(topic),
        .payload     = payload,
        .payload_len = payload_len,
    };

    TickType_t t_start = xTaskGetTickCount();

    cy_rslt_t res = cy_mqtt_publish(mqtt_connection, &pub);

    TickType_t t_end = xTaskGetTickCount();
    uint32_t pub_ms = (uint32_t)(t_end - t_start) * portTICK_PERIOD_MS;
    metrics_record_mqtt_publish_us(pub_ms * 1000u);

    if (pub_ms > 200u)
    {
        printf("[MQTT] WARN: SLOW publish: %lu ms, topic=%s\n", 
               (unsigned long)pub_ms, topic);
    }

    if (res == CY_RSLT_SUCCESS)
    {
        metrics_inc_mqtt_pub_ok();
        s_consec_pub_fails = 0;
        return true;
    }
    else
    {
        metrics_inc_mqtt_pub_fail();
        printf("[MQTT] Publish failed (0x%lX) topic=%s\n",
               (unsigned long)res, topic);

        if (res == CY_RSLT_MODULE_MQTT_PUBLISH_FAIL)
        {
            s_consec_pub_fails++;
        }

        if (gateway_ipc_is_mqtt_online() &&
            !s_disconnect_pending &&
            mqtt_publish_failure_requires_offline(res, s_consec_pub_fails))
        {
            const char *detail =
                (res == CY_RSLT_MODULE_MQTT_NOT_CONNECTED ||
                 res == CY_RSLT_MODULE_MQTT_CLOSED) ?
                MQTT_DISCONNECT_DETAIL_NOT_CONNECTED :
                MQTT_DISCONNECT_DETAIL_PUBLISH_FAIL;
            printf("[MQTT] WARN: forcing offline after publish failure\n");
            mqtt_request_disconnect(detail);
        }
        return false;
    }
}

static bool mqtt_publish_failure_requires_offline(cy_rslt_t res, uint32_t consec_fails)
{
    if (res == CY_RSLT_MODULE_MQTT_NOT_CONNECTED ||
        res == CY_RSLT_MODULE_MQTT_CLOSED)
    {
        return true;
    }

    if (res == CY_RSLT_MODULE_MQTT_PUBLISH_FAIL && consec_fails >= 3u)
    {
        return true;
    }

    return false;
}

static void mqtt_request_disconnect(const char *detail)
{
    s_disconnect_detail = (detail != NULL) ?
        detail : MQTT_DISCONNECT_DETAIL_CALLBACK;
    s_disconnect_pending = true;

    if (s_mqtt_task_handle != NULL)
    {
        (void)xTaskNotifyGive(s_mqtt_task_handle);
    }
}

/*******************************************************************************
 * Backoff helpers
 ******************************************************************************/
static void backoff_reset(void)
{
    s_backoff_ms = g_config.mqtt.backoff_min_ms;
}

static void backoff_wait(void)
{
    printf("[MQTT] Backoff %lu ms\n",
           (unsigned long)s_backoff_ms);

    /* Queue draining is now handled by the dedicated spill task
     * (buffer_task).  We just wait here. */
    vTaskDelay(pdMS_TO_TICKS(s_backoff_ms));

    /* Exponential backoff: double, cap at max */
    s_backoff_ms *= 2u;
    if (s_backoff_ms > g_config.mqtt.backoff_max_ms)
    {
        s_backoff_ms = g_config.mqtt.backoff_max_ms;
    }
}

/*******************************************************************************
 * Command path: subscribe
 ******************************************************************************/
static bool mqtt_subscribe_cmd_topic(void)
{
    char topic[TOPIC_BUF_SIZE];
    snprintf(topic, sizeof(topic), "%s/cmd/request", g_config.mqtt.base_topic);

    /* Store for callback topic matching */
    strncpy(s_cmd_topic, topic, sizeof(s_cmd_topic) - 1u);
    s_cmd_topic[sizeof(s_cmd_topic) - 1u] = '\0';

    cy_mqtt_subscribe_info_t sub = {
        .qos        = (cy_mqtt_qos_t)g_config.mqtt.qos_control,
        .topic      = topic,
        .topic_len  = (uint16_t)strlen(topic),
    };

    cy_rslt_t res = cy_mqtt_subscribe(mqtt_connection, &sub, 1u);
    if (res != CY_RSLT_SUCCESS)
    {
        printf("[MQTT] WARN: subscribe to %s failed (0x%lX)\n",
               topic, (unsigned long)res);
        return false;
    }

    printf("[MQTT] Subscribed to %s\n", topic);
    s_cmd_subscribed = true;
    return true;
}

/*******************************************************************************
 * Command path: publish a response
 ******************************************************************************/
static bool publish_cmd_response(const cmd_response_t *resp)
{
    if (resp == NULL) return false;

    int len = cmd_handler_encode_response(resp, s_cmd_json_buf,
                                          CMD_JSON_BUF_SIZE);
    if (len < 0)
    {
        printf("[MQTT] WARN: cmd response encode failed for id=%s\n",
               resp->id);
        return false;
    }

    char topic[TOPIC_BUF_SIZE];
    snprintf(topic, sizeof(topic), "%s/cmd/response",
             g_config.mqtt.base_topic);

    return publish_control_json(topic, s_cmd_json_buf, (size_t)len);
}

/*******************************************************************************
 * Command path: drain response queue + pending retry
 ******************************************************************************/
static void process_cmd_response_queue(void)
{
    /* 1. Try pending response first (from previous publish failure) */
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
            /* Still can't publish — don't drain new responses */
            return;
        }
    }

    /* 2. Drain cmd_response_queue */
    cmd_response_t resp;
    while (xQueueReceive(gateway_ipc_cmd_response_queue(), &resp, 0)
           == pdTRUE)
    {
        if (publish_cmd_response(&resp))
        {
            cmd_inflight_remove(resp.id);
            cmd_cache_put(&resp);
        }
        else
        {
            /* Publish failed — park in pending slot, stop draining */
            s_cmd_pending_resp  = resp;
            s_cmd_pending_valid = true;
            return;
        }
    }
}

/*******************************************************************************
 * Command path: drain raw queue, parse, dedupe, enqueue requests
 ******************************************************************************/
static void process_cmd_raw_queue(void)
{
    cmd_raw_t raw;

    while (xQueueReceive(gateway_ipc_cmd_raw_queue(), &raw, 0) == pdTRUE)
    {
        cmd_request_t req;
        char id[CMD_ID_MAX];

        cmd_parse_result_t pr = cmd_handler_parse(&raw, &req, id);

        /* --- Drop: no recoverable ID --- */
        if (pr == CMD_PARSE_BAD_JSON)
        {
            printf("[MQTT] WARN: cmd bad JSON, no id — dropped\n");
            continue;
        }

        /* --- Error response with recovered ID --- */
        if (pr == CMD_PARSE_BAD_JSON_WITH_ID ||
            pr == CMD_PARSE_BAD_REQUEST ||
            pr == CMD_PARSE_UNSUPPORTED)
        {
            cmd_status_t st;
            switch (pr)
            {
                case CMD_PARSE_BAD_JSON_WITH_ID: st = CMD_STATUS_BAD_JSON;    break;
                case CMD_PARSE_BAD_REQUEST:      st = CMD_STATUS_BAD_REQUEST; break;
                case CMD_PARSE_UNSUPPORTED:       st = CMD_STATUS_UNSUPPORTED; break;
                default:                          st = CMD_STATUS_BAD_REQUEST; break;
            }

            cmd_response_t err_resp;
            cmd_handler_build_error(&err_resp, id, req.addr_7bit, st);

            if (publish_cmd_response(&err_resp))
            {
                cmd_cache_put(&err_resp);
            }
            else
            {
                /* Park in pending slot — will be retried next loop */
                s_cmd_pending_resp  = err_resp;
                s_cmd_pending_valid = true;
                return;
            }
            continue;
        }

        /* --- CMD_PARSE_OK: valid request --- */

        /* Check dedupe cache first */
        cmd_response_t cached;
        if (cmd_cache_lookup(req.id, &cached))
        {
            (void)publish_cmd_response(&cached);
            /* Cache hit: response already cached, no pending slot needed
             * — loss on publish fail is acceptable (QoS1 client retries). */
            continue;
        }

        /* Check if already in-flight */
        if (cmd_inflight_check(req.id))
        {
            continue;  /* suppress duplicate */
        }

        /* Reserve in-flight slot first (backpressure if tracker full) */
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
                s_cmd_pending_resp  = busy_resp;
                s_cmd_pending_valid = true;
                return;
            }
            continue;
        }

        /* Enqueue to request queue for poll task execution */
        if (xQueueSend(gateway_ipc_cmd_request_queue(), &req, 0) != pdTRUE)
        {
            /* Queue full — undo in-flight reservation and respond */
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
                s_cmd_pending_resp  = full_resp;
                s_cmd_pending_valid = true;
                return;
            }
        }
    }
}

/* [] END OF FILE */

