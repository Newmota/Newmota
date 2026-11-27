/*
  hostap_hash_bearssl.c - hostap HMAC, PBKDF2-SHA1 and SHA-384/512 on BearSSL

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file
 * @brief The hash functions a prebuilt supplicant takes from its crypto backend instead of
 * hostap's md5.c, sha1.c, sha1-pbkdf2.c, sha256.c and sha384/512 sources: the Beken BDK
 * archives were built with crypto_mbedtls.c, which this file and hostap_sae_crypto.c,
 * hostap_symmetric_bearssl.c replace. ESP-IDF compiles those hostap sources itself, so
 * this file is not part of an ESP build (its symbols would be defined twice).
 *
 * HMAC keeps BearSSL's precomputed inner and outer states, so each PBKDF2 iteration costs
 * two SHA-1 compressions. Every function returns 0; hostap's -1 cases (allocation, bad
 * hash) cannot occur here, except PBKDF2 with fewer than one iteration.
 */
#include "utils/includes.h"
#include "utils/common.h"
#include "crypto/md5.h"
#include "crypto/sha1.h"
#include "crypto/sha256.h"
#include "crypto/crypto.h"

/* As in the BDK's crypto/sha384.h and sha512.h, which ESP-IDF's hostap (the host test's
 * headers) does not ship. */
int hmac_sha384_vector(
    const u8* key, size_t key_len, size_t num_elem, const u8* addr[], const size_t* len, u8* mac);
int hmac_sha384(const u8* key, size_t key_len, const u8* data, size_t data_len, u8* mac);
int hmac_sha512_vector(
    const u8* key, size_t key_len, size_t num_elem, const u8* addr[], const size_t* len, u8* mac);
int hmac_sha512(const u8* key, size_t key_len, const u8* data, size_t data_len, u8* mac);

#include <string.h>

#include "bearssl_hash.h"
#include "bearssl_hmac.h"
#include "newmota_crypto.h"
#if defined(NEWMOTA_HOSTAP_HW_PBKDF2_SHA1)
#include "newmota_features.h"
#include "newmota/platform/crypto_hash.h"
#endif

static int hmac_vector(const br_hash_class* hash, const u8* key, size_t key_len, size_t num_elem,
    const u8* addr[], const size_t* len, u8* mac) {
    br_hmac_key_context kc;
    br_hmac_context hc;
    br_hmac_key_init(&kc, hash, key, key_len);
    br_hmac_init(&hc, &kc, 0);
    for (size_t i = 0; i < num_elem; i++) {
        br_hmac_update(&hc, addr[i], len[i]);
    }
    br_hmac_out(&hc, mac);
    nm_crypto_zero(&hc, sizeof(hc));
    nm_crypto_zero(&kc, sizeof(kc));
    return 0;
}

static int hash_vector(
    const br_hash_class* hash, size_t num_elem, const u8* addr[], const size_t* len, u8* mac) {
    br_hash_compat_context ctx;
    hash->init(&ctx.vtable);
    for (size_t i = 0; i < num_elem; i++) {
        hash->update(&ctx.vtable, addr[i], len[i]);
    }
    hash->out(&ctx.vtable, mac);
    nm_crypto_zero(&ctx, sizeof(ctx));
    return 0;
}

int hmac_md5_vector(
    const u8* key, size_t key_len, size_t num_elem, const u8* addr[], const size_t* len, u8* mac) {
    return hmac_vector(&br_md5_vtable, key, key_len, num_elem, addr, len, mac);
}

int hmac_md5(const u8* key, size_t key_len, const u8* data, size_t data_len, u8* mac) {
    return hmac_md5_vector(key, key_len, 1, &data, &data_len, mac);
}

int hmac_sha1_vector(
    const u8* key, size_t key_len, size_t num_elem, const u8* addr[], const size_t* len, u8* mac) {
    return hmac_vector(&br_sha1_vtable, key, key_len, num_elem, addr, len, mac);
}

int hmac_sha1(const u8* key, size_t key_len, const u8* data, size_t data_len, u8* mac) {
    return hmac_sha1_vector(key, key_len, 1, &data, &data_len, mac);
}

int hmac_sha256_vector(
    const u8* key, size_t key_len, size_t num_elem, const u8* addr[], const size_t* len, u8* mac) {
    return hmac_vector(&br_sha256_vtable, key, key_len, num_elem, addr, len, mac);
}

int hmac_sha256(const u8* key, size_t key_len, const u8* data, size_t data_len, u8* mac) {
    return hmac_sha256_vector(key, key_len, 1, &data, &data_len, mac);
}

int hmac_sha384_vector(
    const u8* key, size_t key_len, size_t num_elem, const u8* addr[], const size_t* len, u8* mac) {
    return hmac_vector(&br_sha384_vtable, key, key_len, num_elem, addr, len, mac);
}

int hmac_sha384(const u8* key, size_t key_len, const u8* data, size_t data_len, u8* mac) {
    return hmac_sha384_vector(key, key_len, 1, &data, &data_len, mac);
}

int hmac_sha512_vector(
    const u8* key, size_t key_len, size_t num_elem, const u8* addr[], const size_t* len, u8* mac) {
    return hmac_vector(&br_sha512_vtable, key, key_len, num_elem, addr, len, mac);
}

int hmac_sha512(const u8* key, size_t key_len, const u8* data, size_t data_len, u8* mac) {
    return hmac_sha512_vector(key, key_len, 1, &data, &data_len, mac);
}

int sha384_vector(size_t num_elem, const u8* addr[], const size_t* len, u8* mac) {
    return hash_vector(&br_sha384_vtable, num_elem, addr, len, mac);
}

int sha512_vector(size_t num_elem, const u8* addr[], const size_t* len, u8* mac) {
    return hash_vector(&br_sha512_vtable, num_elem, addr, len, mac);
}

/* RFC 8018 PBKDF2 with HMAC-SHA1, as hostap's sha1-pbkdf2.c: blocks T_1, T_2, ... of
 * 20 bytes, the last one truncated to buflen. */
int pbkdf2_sha1(const char* passphrase, const u8* ssid, size_t ssid_len, int iterations, u8* buf,
    size_t buflen) {
    br_hmac_key_context kc;
    br_hmac_context hc;
    u8 u[SHA1_MAC_LEN];
    u8 t[SHA1_MAC_LEN];
    u8 count[4];
    u32 block = 0;
    if (iterations < 1) {
        return -1;
    }
#if defined(NEWMOTA_HOSTAP_HW_PBKDF2_SHA1)
    /* Bouffalo: midstates on the SHA engine, held for 64 iterations (crypto_hash.c). */
    if (newmota_hw_pbkdf2_sha1(passphrase, ssid, ssid_len, iterations, buf, buflen) == 0) {
        return 0;
    }
#endif
    br_hmac_key_init(&kc, &br_sha1_vtable, passphrase, os_strlen(passphrase));
    while (buflen > 0) {
        size_t take = buflen < SHA1_MAC_LEN ? buflen : SHA1_MAC_LEN;
        block++;
        WPA_PUT_BE32(count, block);
        br_hmac_init(&hc, &kc, 0);
        br_hmac_update(&hc, ssid, ssid_len);
        br_hmac_update(&hc, count, sizeof(count));
        br_hmac_out(&hc, u);
        memcpy(t, u, sizeof(t));
        for (int i = 1; i < iterations; i++) {
            br_hmac_init(&hc, &kc, 0);
            br_hmac_update(&hc, u, sizeof(u));
            br_hmac_out(&hc, u);
            for (size_t j = 0; j < sizeof(t); j++) {
                t[j] ^= u[j];
            }
        }
        memcpy(buf, t, take);
        buf += take;
        buflen -= take;
    }
    nm_crypto_zero(&hc, sizeof(hc));
    nm_crypto_zero(&kc, sizeof(kc));
    nm_crypto_zero(u, sizeof(u));
    nm_crypto_zero(t, sizeof(t));
    return 0;
}
