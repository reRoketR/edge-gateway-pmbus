/******************************************************************************
* File Name:   mqtt_client_config.h
*
* Description: MQTT client configuration for PMBus-MQTT Gateway.
*              Configures broker connection, topics, and security.
*
*******************************************************************************/

#ifndef MQTT_CLIENT_CONFIG_H_
#define MQTT_CLIENT_CONFIG_H_

#include "cy_mqtt_api.h"

/*******************************************************************************
* Macros
********************************************************************************/

/***************** MQTT CLIENT CONNECTION CONFIGURATION MACROS *****************/

/* MQTT Broker/Server address and port. */
#define MQTT_BROKER_ADDRESS               "192.168.1.2"
#define MQTT_PORT                         1883

/* Set this macro to 1 if a secure (TLS) connection to the MQTT Broker is
 * required to be established, else 0.
 */
#define MQTT_SECURE_CONNECTION            (0)

/* Configure the user credentials to be sent as part of MQTT CONNECT packet */
#define MQTT_USERNAME                     ""
#define MQTT_PASSWORD                     ""

/********************* MQTT MESSAGE CONFIGURATION MACROS **********************/

/* Base topic for PMBus gateway messages.
 * Full topics are constructed at runtime:
 *   pmbus/<gw_id>/dev/<addr>/telemetry
 *   pmbus/<gw_id>/dev/<addr>/status
 *   pmbus/<gw_id>/metrics
 *   pmbus/<gw_id>/events
 */
#define MQTT_BASE_TOPIC                   "pmbus/gw01"

/* Default QoS for MQTT publish and subscribe.
 * Valid choices are 0, 1, and 2.
 */
#define MQTT_MESSAGES_QOS                 (1)

/* Last Will and Testament (LWT) configuration. */
#define ENABLE_LWT_MESSAGE                (1)
#if ENABLE_LWT_MESSAGE
    #define MQTT_WILL_TOPIC_NAME          MQTT_BASE_TOPIC "/events"
    #define MQTT_WILL_MESSAGE             ("{\"type\":\"GATEWAY_UNEXPECTED_DISCONNECT\",\"detail\":\"lwt\"}")
#endif

/******************* OTHER MQTT CLIENT CONFIGURATION MACROS *******************/

/* A unique client identifier for every MQTT connection. */
#define MQTT_CLIENT_IDENTIFIER            "pmbus-gw01"

/* The timeout in milliseconds for MQTT operations. */
#define MQTT_TIMEOUT_MS                   (5000)

/* The keep-alive interval in seconds used for MQTT ping request. */
#define MQTT_KEEP_ALIVE_SECONDS           (60)

/* Generate unique client ID by appending timestamp. */
#define GENERATE_UNIQUE_CLIENT_ID         (1)

/* The longest client identifier that an MQTT server must accept. */
#define MQTT_CLIENT_IDENTIFIER_MAX_LEN    (23)

/* SNI Host Name (used for TLS only). */
#define MQTT_SNI_HOSTNAME                 (MQTT_BROKER_ADDRESS)

/* Network buffer size for MQTT send and receive operations. */
#define MQTT_NETWORK_BUFFER_SIZE          (2 * CY_MQTT_MIN_NETWORK_BUFFER_SIZE)

/* Maximum MQTT connection re-connection limit. */
#define MAX_MQTT_CONN_RETRIES             (150u)

/* MQTT re-connection time interval in milliseconds. */
#define MQTT_CONN_RETRY_INTERVAL_MS       (2000)

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

#endif /* MQTT_CLIENT_CONFIG_H_ */
