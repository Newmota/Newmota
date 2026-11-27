/*
  crypto_hash.h - Hardware compression boundary for BearSSL SHA

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file
 * @brief Hardware SHA compression with BearSSL's native-word chaining state.
 * Selected providers must accept arbitrary state, including an imported IV.
 * One call covers a whole run of blocks, so a provider pays its engine handover once
 * per run, not per block. Calls complete synchronously. A hardware failure must stop execution:
 * BearSSL's hash interface has no failure return, and returning a substitute digest is unsafe.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Compress @p num consecutive SHA-1 blocks through hardware; @p num is at least 1. */
void newmota_sha1_blocks(const unsigned char* buf, size_t num, uint32_t* val);
/** @brief Compress @p num consecutive SHA-224/256 blocks through hardware. */
void newmota_sha256_blocks(const unsigned char* buf, size_t num, uint32_t* val);
/** @brief Compress @p num consecutive SHA-384/512 blocks through hardware. */
void newmota_sha512_blocks(const unsigned char* buf, size_t num, uint64_t* val);
#if (defined(BL602) || defined(BL616) || defined(BL618DG)) && defined(USE_CRYPTO_HW_SHA1) && \
    USE_CRYPTO_HW_SHA1 && defined(USE_WIFI_WPA3_BEARSSL) && USE_WIFI_WPA3_BEARSSL
/** @brief Bouffalo PBKDF2-HMAC-SHA1 (hostap pbkdf2_sha1()) on the engine with HMAC midstates.
 * Returns 0 with @p buf filled, or 1 without touching it when the arguments need the
 * generic version (fewer than one iteration, SSID over 51 bytes). */
int newmota_hw_pbkdf2_sha1(const char* passphrase, const unsigned char* ssid, size_t ssid_len,
    int iterations, unsigned char* buf, size_t buflen);
#endif

#ifdef __cplusplus
}
#endif
