/******************************************************************************
* File Name:   mqtt_client_config.h
*
* Description: MQTT transport-level configuration for the Infineon MQTT library.
*
*              Broker host, port, client ID, base topic, QoS, and LWT content
*              are all determined at runtime from g_config (see gateway_config.h
*              and the active profile header in source/profiles/).
*
*              This file contains ONLY library-level transport parameters:
*              buffer sizes, keepalive, TLS placeholders, retry limits.
*
*******************************************************************************/

#ifndef MQTT_CLIENT_CONFIG_H_
#define MQTT_CLIENT_CONFIG_H_

#include "cy_mqtt_api.h"

/*******************************************************************************
* Transport parameters (library-level, not application-level)
********************************************************************************/

/* Set this macro to 1 if a secure (TLS) connection to the MQTT Broker is
 * required to be established, else 0.
 */
#define MQTT_SECURE_CONNECTION            (0)

/* The timeout in milliseconds for MQTT operations. */
#define MQTT_TIMEOUT_MS                   (2000)

/* The keep-alive interval in seconds used for MQTT ping request. */
#define MQTT_KEEP_ALIVE_SECONDS           (15)

/* Generate unique client ID by appending timestamp. */
#define GENERATE_UNIQUE_CLIENT_ID         (1)

/* The longest client identifier that an MQTT server must accept. */
#define MQTT_CLIENT_IDENTIFIER_MAX_LEN    (23)

/* Network buffer size for MQTT send and receive operations. */
#define MQTT_NETWORK_BUFFER_SIZE          (2 * CY_MQTT_MIN_NETWORK_BUFFER_SIZE)

/* Maximum MQTT connection re-connection limit. */
#define MAX_MQTT_CONN_RETRIES             (150u)

/* MQTT re-connection time interval in milliseconds. */
#define MQTT_CONN_RETRY_INTERVAL_MS       (2000)

/* Enable Last Will and Testament (LWT).
 * The LWT topic and payload are built at runtime from g_config
 * (see mqtt_gw_task.c → mqtt_init_and_create). */
#define ENABLE_LWT_MESSAGE                (1)

/**************** MQTT CLIENT CERTIFICATE CONFIGURATION MACROS ****************/

/* Configure the below credentials in case of a secure MQTT connection. */
#define CLIENT_CERTIFICATE      \
"-----BEGIN CERTIFICATE-----\n"\
"........base64 data........\n"\
"-----END CERTIFICATE-----"

#define CLIENT_PRIVATE_KEY          \
"-----BEGIN RSA PRIVATE KEY-----\n"\
"..........base64 data..........\n"\
"-----END RSA PRIVATE KEY-----"

#define ROOT_CA_CERTIFICATE     \
"-----BEGIN CERTIFICATE-----\n"\
"........base64 data........\n"\
"-----END CERTIFICATE-----"

/******************************************************************************
* Global Variables
*******************************************************************************/
extern cy_mqtt_broker_info_t broker_info;
extern cy_awsport_ssl_credentials_t  *security_info;
extern cy_mqtt_connect_info_t connection_info;

#if ENABLE_LWT_MESSAGE
/**
 * @brief Set LWT topic and payload at runtime from g_config.
 *
 * Must be called before cy_mqtt_connect() so the broker receives the
 * correct will message.  Called from mqtt_gw_task.c.
 *
 * @param[in] topic    LWT topic string (e.g. "pmbus/gw01/events")
 * @param[in] payload  LWT JSON payload
 */
void mqtt_config_set_lwt(const char *topic, const char *payload);
#endif

#endif /* MQTT_CLIENT_CONFIG_H_ */
