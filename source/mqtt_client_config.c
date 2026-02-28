/******************************************************************************
* File Name:   mqtt_client_config.c
*
* Description: MQTT client configuration structures for PMBus-MQTT Gateway.
*              Based on Infineon Wi-Fi MQTT Client example (CE229889).
*
*******************************************************************************/

#include <stdio.h>
#include "mqtt_client_config.h"
#include "cy_mqtt_api.h"

/******************************************************************************
* Global Variables
*******************************************************************************/

/* MQTT Broker/Server details */
cy_mqtt_broker_info_t broker_info =
{
    .hostname = MQTT_BROKER_ADDRESS,
    .hostname_len = sizeof(MQTT_BROKER_ADDRESS) - 1,
    .port = MQTT_PORT
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
 * NOTE: topic/payload use compile-time strings. At runtime,
 * mqtt_gw_task overrides broker host/port from g_config, but LWT
 * topic uses the same base topic. If profiles change base_topic,
 * update MQTT_BASE_TOPIC accordingly (or build LWT topic at runtime). */
static cy_mqtt_publish_info_t will_msg_info =
{
    .qos = CY_MQTT_QOS1,
    .topic = MQTT_WILL_TOPIC_NAME,
    .topic_len = (uint16_t)(sizeof(MQTT_WILL_TOPIC_NAME) - 1),
    .payload = MQTT_WILL_MESSAGE,
    .payload_len = (size_t)(sizeof(MQTT_WILL_MESSAGE) - 1),
    .retain = false,
    .dup = false
};
#endif /* ENABLE_LWT_MESSAGE */

/* MQTT connection information structure */
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

/* Check for a valid QoS setting */
#if ((MQTT_MESSAGES_QOS != 0) && (MQTT_MESSAGES_QOS != 1) && (MQTT_MESSAGES_QOS != 2))
    #error "Invalid QoS setting! MQTT_MESSAGES_QOS must be either 0 or 1 or 2."
#endif
