/*
  mbedtls_config.h - bounded Mbed TLS features for Pico HTTPS

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file mbedtls_config.h
 * @brief Bounded Mbed TLS features for Pico HTTPS.
 */

#pragma once

/* Work around Mbed TLS source files that use INT_MAX without including limits.h. */
#include <limits.h>

#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_SSL_OUT_CONTENT_LEN 2048
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SHA256_SMALLER
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_AES_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_ERROR_C
#define MBEDTLS_GCM_C
#define MBEDTLS_MD_C
#define MBEDTLS_OID_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_PEM_WRITE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CRT_WRITE_C
#define MBEDTLS_X509_CREATE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_SSL_PROTO_TLS1_2

#define MBEDTLS_PLATFORM_MS_TIME_ALT
