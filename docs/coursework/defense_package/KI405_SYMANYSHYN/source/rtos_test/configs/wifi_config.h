/******************************************************************************
* File Name: wifi_config.h
*
* Description: Wi-Fi connection configuration for PMBus-MQTT Gateway.
*              This is a TEMPLATE with placeholder values.
*
*              To use: copy this file to configs/wifi_config_local.h and
*              fill in your real SSID / password.  That file is git-ignored
*              and will never be committed.
*
*******************************************************************************/

#ifndef WIFI_CONFIG_H_
#define WIFI_CONFIG_H_

#include "cy_wcm.h"

/*******************************************************************************
* Macros
********************************************************************************/

/*
 * If a local (git-ignored) override exists, use it.
 * Otherwise fall through to the placeholders below — build will succeed
 * but Wi-Fi will not connect until you create the local file.
 */
#if __has_include("wifi_config_local.h")
  #include "wifi_config_local.h"
#else

  #warning "Using placeholder Wi-Fi credentials.  Copy configs/wifi_config.h " \
           "to configs/wifi_config_local.h and set your real SSID/password."

  /* SSID of the Wi-Fi Access Point to which the gateway connects. */
  #define WIFI_SSID                         "<YOUR_SSID>"

  /* Passkey of the above mentioned Wi-Fi SSID. */
  #define WIFI_PASSWORD                     "<YOUR_PASSWORD>"

  /* Security type of the Wi-Fi access point. See 'cy_wcm_security_t' structure
   * in "cy_wcm.h" for more details.
   */
  #define WIFI_SECURITY                     CY_WCM_SECURITY_WPA2_AES_PSK

#endif /* __has_include */

/* Maximum Wi-Fi re-connection limit. */
#ifndef MAX_WIFI_CONN_RETRIES
  #define MAX_WIFI_CONN_RETRIES             (120u)
#endif

/* Wi-Fi re-connection time interval in milliseconds. */
#ifndef WIFI_CONN_RETRY_INTERVAL_MS
  #define WIFI_CONN_RETRY_INTERVAL_MS       (5000)
#endif

#endif /* WIFI_CONFIG_H_ */
