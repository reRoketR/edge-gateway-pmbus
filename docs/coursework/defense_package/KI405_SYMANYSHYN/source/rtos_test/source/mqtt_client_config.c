/******************************************************************************
* File Name:   mqtt_client_config.c
*
* Description: MQTT client configuration structures for PMBus-MQTT Gateway.
*              Based on Infineon Wi-Fi MQTT Client example (CE229889).
*
*              Broker host, port, client ID, LWT topic/payload are all set
*              at runtime from g_config by mqtt_gw_task.c before connect.
*              This file initialises the structures to safe defaults.
*
*******************************************************************************/

#include <stdio.h>
#include <string.h>
#include "mqtt_client_config.h"
#include "cy_mqtt_api.h"

/******************************************************************************
* Global Variables
*******************************************************************************/

/* MQTT Broker/Server details.
 * hostname and port are overridden from g_config by mqtt_gw_task.c
 * in mqtt_init_and_create() before cy_mqtt_create() is called. */
cy_mqtt_broker_info_t broker_info =
{
    .hostname = "",
    .hostname_len = 0,
    .port = 0
};

#if (MQTT_SECURE_CONNECTION)
/* MQTT client credentials for secure connection. */
static cy_awsport_ssl_credentials_t credentials =
{
#ifdef CLIENT_CERTIFICATE
    .client_cert = (const char *)CLIENT_CERTIFICATE,
    .client_cert_size = sizeof(CLIENT_CERTIFICATE),
#else
    .client_cert = NULL,
    .client_cert_size = 0,
#endif

#ifdef CLIENT_PRIVATE_KEY
    .private_key = (const char *)CLIENT_PRIVATE_KEY,
    .private_key_size = sizeof(CLIENT_PRIVATE_KEY),
#else
    .private_key = NULL,
    .private_key_size = 0,
#endif

#ifdef ROOT_CA_CERTIFICATE
    .root_ca = (const char *)ROOT_CA_CERTIFICATE,
    .root_ca_size = sizeof(ROOT_CA_CERTIFICATE),
#else
    .root_ca = NULL,
    .root_ca_size = 0,
#endif

#ifdef MQTT_ALPN_PROTOCOL_NAME
    .alpnprotos = (const char *)MQTT_ALPN_PROTOCOL_NAME,
    .alpnprotoslen = sizeof(MQTT_ALPN_PROTOCOL_NAME),
#else
    .alpnprotos = NULL,
    .alpnprotoslen = 0,
#endif

#ifdef MQTT_SNI_HOSTNAME
    .sni_host_name = (const char *)MQTT_SNI_HOSTNAME,
    .sni_host_name_size = sizeof(MQTT_SNI_HOSTNAME)
#else
    .sni_host_name = NULL,
    .sni_host_name_size = 0
#endif
};

cy_awsport_ssl_credentials_t *security_info = &credentials;

#else
cy_awsport_ssl_credentials_t *security_info = NULL;
#endif /* MQTT_SECURE_CONNECTION */

#if ENABLE_LWT_MESSAGE
/* Last Will and Testament (LWT) message structure.
 * Topic and payload are built at runtime from g_config by mqtt_gw_task.c
 * (mqtt_init_and_create → setup_lwt).  The static buffers below are
 * populated before cy_mqtt_connect() is called. */
static char lwt_topic_buf[96];
static char lwt_payload_buf[128];

static cy_mqtt_publish_info_t will_msg_info =
{
    .qos = CY_MQTT_QOS1,
    .topic = lwt_topic_buf,
    .topic_len = 0,
    .payload = lwt_payload_buf,
    .payload_len = 0,
    .retain = false,
    .dup = false
};

void mqtt_config_set_lwt(const char *topic, const char *payload)
{
    if (topic != NULL)
    {
        strncpy(lwt_topic_buf, topic, sizeof(lwt_topic_buf) - 1u);
        lwt_topic_buf[sizeof(lwt_topic_buf) - 1u] = '\0';
        will_msg_info.topic_len = (uint16_t)strlen(lwt_topic_buf);
    }
    if (payload != NULL)
    {
        strncpy(lwt_payload_buf, payload, sizeof(lwt_payload_buf) - 1u);
        lwt_payload_buf[sizeof(lwt_payload_buf) - 1u] = '\0';
        will_msg_info.payload_len = strlen(lwt_payload_buf);
    }
}
#endif /* ENABLE_LWT_MESSAGE */

/* MQTT connection information structure.
 * client_id is overridden from g_config by mqtt_gw_task.c in
 * mqtt_broker_connect().  LWT will_info is populated via
 * mqtt_config_set_lwt() before connect. */
cy_mqtt_connect_info_t connection_info =
{
    .client_id = NULL,
    .client_id_len = 0,
    .username = NULL,
    .username_len = 0,
    .password = NULL,
    .password_len = 0,
    .clean_session = true,
    .keep_alive_sec = MQTT_KEEP_ALIVE_SECONDS,
#if ENABLE_LWT_MESSAGE
    .will_info = &will_msg_info
#else
    .will_info = NULL
#endif
};
