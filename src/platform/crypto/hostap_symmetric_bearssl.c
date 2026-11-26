/*
  hostap_symmetric_bearssl.c - hostap MD5, SHA-1, SHA-256 and AES block primitives on BearSSL

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file
 * @brief Replaces the supplicant's internal md5/sha1/sha256 and AES implementations in the
 * ESP-IDF fork's CONFIG_ESP_WIFI_EXTERNAL_CRYPTO build. hostap's generic HMAC, PBKDF2, KDF,
 * CCMP, OMAC1, key wrap and CTR/CBC code stays and calls these. The BearSSL hashes and
 * br_aes_big use the selected hardware providers (USE_CRYPTO_HW_SHA1/SHA256/AES).
 * Single AES blocks run as one-block CBC with a zero IV, which equals ECB.
 * NEWMOTA_HOSTAP_HASH_ONLY (BL602, whose supplicant keeps its own wpa_aes_* functions)
 * builds only the hash vectors.
 */
#include "utils/includes.h"
#include "utils/common.h"
#if !(defined(NEWMOTA_HOSTAP_HASH_ONLY) && NEWMOTA_HOSTAP_HASH_ONLY)
#include "crypto/aes.h"
#endif
#include "crypto/crypto.h"

#include <string.h>

#include "bearssl_block.h"
#include "bearssl_hash.h"
#include "newmota_crypto.h"

int md5_vector(size_t num_elem, const u8* addr[], const size_t* len, u8* mac) {
    br_md5_context ctx;
    br_md5_init(&ctx);
    for (size_t i = 0; i < num_elem; i++) {
        br_md5_update(&ctx, addr[i], len[i]);
    }
    br_md5_out(&ctx, mac);
    nm_crypto_zero(&ctx, sizeof(ctx));
    return 0;
}

int sha1_vector(size_t num_elem, const u8* addr[], const size_t* len, u8* mac) {
    br_sha1_context ctx;
    br_sha1_init(&ctx);
    for (size_t i = 0; i < num_elem; i++) {
        br_sha1_update(&ctx, addr[i], len[i]);
    }
    br_sha1_out(&ctx, mac);
    nm_crypto_zero(&ctx, sizeof(ctx));
    return 0;
}

int sha256_vector(size_t num_elem, const u8* addr[], const size_t* len, u8* mac) {
    br_sha256_context ctx;
    br_sha256_init(&ctx);
    for (size_t i = 0; i < num_elem; i++) {
        br_sha256_update(&ctx, addr[i], len[i]);
    }
    br_sha256_out(&ctx, mac);
    nm_crypto_zero(&ctx, sizeof(ctx));
    return 0;
}

#if !(defined(NEWMOTA_HOSTAP_HASH_ONLY) && NEWMOTA_HOSTAP_HASH_ONLY)

void* aes_encrypt_init(const u8* key, size_t len) {
    br_aes_big_cbcenc_keys* ctx;
    if (len != 16 && len != 24 && len != 32) {
        return NULL;
    }
    ctx = os_malloc(sizeof(*ctx));
    if (ctx != NULL) {
        br_aes_big_cbcenc_init(ctx, key, len);
    }
    return ctx;
}

int aes_encrypt(void* ctx, const u8* plain, u8* crypt) {
    u8 iv[16] = {0};
    memmove(crypt, plain, 16);
    br_aes_big_cbcenc_run((const br_aes_big_cbcenc_keys*)ctx, iv, crypt, 16);
    return 0;
}

void aes_encrypt_deinit(void* ctx) {
    if (ctx != NULL) {
        nm_crypto_zero(ctx, sizeof(br_aes_big_cbcenc_keys));
        os_free(ctx);
    }
}

void* aes_decrypt_init(const u8* key, size_t len) {
    br_aes_big_cbcdec_keys* ctx;
    if (len != 16 && len != 24 && len != 32) {
        return NULL;
    }
    ctx = os_malloc(sizeof(*ctx));
    if (ctx != NULL) {
        br_aes_big_cbcdec_init(ctx, key, len);
    }
    return ctx;
}

int aes_decrypt(void* ctx, const u8* crypt, u8* plain) {
    u8 iv[16] = {0};
    memmove(plain, crypt, 16);
    br_aes_big_cbcdec_run((const br_aes_big_cbcdec_keys*)ctx, iv, plain, 16);
    return 0;
}

void aes_decrypt_deinit(void* ctx) {
    if (ctx != NULL) {
        nm_crypto_zero(ctx, sizeof(br_aes_big_cbcdec_keys));
        os_free(ctx);
    }
}
#endif
