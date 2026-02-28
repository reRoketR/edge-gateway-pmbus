/**
 * \file mbedtls_user_config.h
 *
 * \brief mbedTLS configuration for PMBus-MQTT Gateway.
 *        Based on Infineon Wi-Fi MQTT Client example (CE229889).
 *        Optimized for non-TLS MQTT (port 1883) with minimal footprint.
 */
/*
 *  Copyright (C) 2006-2018, ARM Limited, All Rights Reserved
 *  SPDX-License-Identifier: Apache-2.0
 */

#ifndef MBEDTLS_USER_CONFIG_HEADER
#define MBEDTLS_USER_CONFIG_HEADER

#if !defined(COMPONENT_4390X)
#include "cy_syslib.h"
#endif

/* Workaround for Cortex-M0+/M23 with ARMC6 optimization */
#if defined(COMPONENT_CM0P) && defined(COMPONENT_ARM)
#define MULADDC_CANNOT_USE_R7
#endif

/* IAR assembly workaround */
#if defined(__IAR_SYSTEMS_ICC__)
#undef MBEDTLS_HAVE_ASM
#endif

/* ---- Disable features not needed for non-TLS MQTT ---- */
#undef MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_TIME_ALT
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* Elliptic curves - keep only required ones */
#undef MBEDTLS_ECP_DP_SECP192R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP224R1_ENABLED
/* SECP256R1 enabled by default */
#undef MBEDTLS_ECP_DP_SECP384R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP521R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP192K1_ENABLED
#undef MBEDTLS_ECP_DP_SECP224K1_ENABLED
#undef MBEDTLS_ECP_DP_SECP256K1_ENABLED
#undef MBEDTLS_ECP_DP_BP256R1_ENABLED
#undef MBEDTLS_ECP_DP_BP384R1_ENABLED
#undef MBEDTLS_ECP_DP_BP512R1_ENABLED
#undef MBEDTLS_ECP_DP_CURVE448_ENABLED

/* Disable PSK and other unused key exchange */
#undef MBEDTLS_KEY_EXCHANGE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_DHE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECDHE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_RSA_PSK_ENABLED
#undef MBEDTLS_PK_PARSE_EC_EXTENDED

/* Disable filesystem and platform entropy (we use HW entropy) */
#undef MBEDTLS_FS_IO
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_FORCE_SHA256

/* Disable server mode */
#undef MBEDTLS_SSL_SRV_C

/* Disable unused ciphers */
#undef MBEDTLS_CHACHA20_C
#undef MBEDTLS_CHACHAPOLY_C
#undef MBEDTLS_POLY1305_C
#undef MBEDTLS_CAMELLIA_C

/* Disable unused features */
#undef MBEDTLS_SELF_TEST
#undef MBEDTLS_SSL_FALLBACK_SCSV
#undef MBEDTLS_SSL_CBC_RECORD_SPLITTING
#undef MBEDTLS_SSL_RENEGOTIATION
#undef MBEDTLS_SSL_PROTO_TLS1
#undef MBEDTLS_SSL_PROTO_TLS1_1
#undef MBEDTLS_SSL_PROTO_DTLS
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID_COMPAT
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#undef MBEDTLS_SSL_DTLS_BADMAC_LIMIT
#undef MBEDTLS_SSL_EXPORT_KEYS
#undef MBEDTLS_SSL_TRUNCATED_HMAC

/* Disable unused modules */
#undef MBEDTLS_AESNI_C
#undef MBEDTLS_NET_C
#undef MBEDTLS_SSL_COOKIE_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_X509_CRL_PARSE_C
#undef MBEDTLS_X509_CSR_PARSE_C
#undef MBEDTLS_X509_CREATE_C
#undef MBEDTLS_X509_CSR_WRITE_C
#undef MBEDTLS_X509_CRT_WRITE_C
#undef MBEDTLS_CERTS_C
#undef MBEDTLS_ERROR_C
#undef MBEDTLS_PADLOCK_C
#undef MBEDTLS_RIPEMD160_C
#undef MBEDTLS_ARC4_C
#undef MBEDTLS_XTEA_C
#undef MBEDTLS_BLOWFISH_C

/* Disable PSA crypto storage (no filesystem) */
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C

/* TLS 1.3 support */
#define MBEDTLS_SSL_PROTO_TLS1_3

#ifndef MBEDTLS_SSL_PROTO_TLS1_3
#undef MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#endif

/* Session tickets required when TLS1.3 is enabled */
#ifndef MBEDTLS_SSL_PROTO_TLS1_3
#undef MBEDTLS_SSL_SESSION_TICKETS
#endif

#ifdef MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_PK_RSA_ALT_SUPPORT
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#endif

/* Deprecation handling */
#define MBEDTLS_DEPRECATED_REMOVED

/* Debug logging - disabled by default */
#define MBEDTLS_VERBOSE 0
#undef MBEDTLS_DEBUG_C

/* Disable unused modules */
#undef MBEDTLS_LMS_C
#undef MBEDTLS_PKCS7_C

/* Default TLS version for server */
#define FORCE_TLS_VERSION MBEDTLS_SSL_VERSION_TLS1_3

/* Hardware acceleration */
#ifndef DISABLE_MBEDTLS_ACCELERATION
#include "mbedtls_alt_config.h"

/* MBEDTLS defines for Dcache supported platforms */
#if !defined(CY_DISABLE_XMC7000_DATA_CACHE) && defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_MEMORY_BUFFER_ALLOC_C
#define MBEDTLS_THREADING_ALT
#define MBEDTLS_THREADING_C
#endif

/* Disable ECP_ALT for unsupported curves */
#ifdef MBEDTLS_ECP_DP_SECP192K1_ENABLED
#undef MBEDTLS_ECP_ALT
#undef MBEDTLS_ECDH_GEN_PUBLIC_ALT
#undef MBEDTLS_ECDSA_SIGN_ALT
#undef MBEDTLS_ECDSA_VERIFY_ALT
#endif
#ifdef MBEDTLS_ECP_DP_SECP224K1_ENABLED
#undef MBEDTLS_ECP_ALT
#undef MBEDTLS_ECDH_GEN_PUBLIC_ALT
#undef MBEDTLS_ECDSA_SIGN_ALT
#undef MBEDTLS_ECDSA_VERIFY_ALT
#endif
#ifdef MBEDTLS_ECP_DP_SECP256K1_ENABLED
#undef MBEDTLS_ECP_ALT
#undef MBEDTLS_ECDH_GEN_PUBLIC_ALT
#undef MBEDTLS_ECDSA_SIGN_ALT
#undef MBEDTLS_ECDSA_VERIFY_ALT
#endif
#ifdef MBEDTLS_ECP_DP_BP256R1_ENABLED
#undef MBEDTLS_ECP_ALT
#undef MBEDTLS_ECDH_GEN_PUBLIC_ALT
#undef MBEDTLS_ECDSA_SIGN_ALT
#undef MBEDTLS_ECDSA_VERIFY_ALT
#endif
#ifdef MBEDTLS_ECP_DP_BP384R1_ENABLED
#undef MBEDTLS_ECP_ALT
#undef MBEDTLS_ECDH_GEN_PUBLIC_ALT
#undef MBEDTLS_ECDSA_SIGN_ALT
#undef MBEDTLS_ECDSA_VERIFY_ALT
#endif
#ifdef MBEDTLS_ECP_DP_BP512R1_ENABLED
#undef MBEDTLS_ECP_ALT
#undef MBEDTLS_ECDH_GEN_PUBLIC_ALT
#undef MBEDTLS_ECDSA_SIGN_ALT
#undef MBEDTLS_ECDSA_VERIFY_ALT
#endif
#ifdef MBEDTLS_ECP_DP_CURVE25519_ENABLED
#undef MBEDTLS_ECP_ALT
#undef MBEDTLS_ECDH_GEN_PUBLIC_ALT
#undef MBEDTLS_ECDSA_SIGN_ALT
#undef MBEDTLS_ECDSA_VERIFY_ALT
#endif

#endif /* DISABLE_MBEDTLS_ACCELERATION */

#endif /* MBEDTLS_USER_CONFIG_HEADER */
