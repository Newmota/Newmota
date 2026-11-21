/* Copyright (C) 2026 Newmota contributors. SPDX-License-Identifier: GPL-3.0-or-later */
#include "newmota_crypto.h"
#include "bearssl.h"
#include <string.h>

int nm_pbkdf2_sha256(const void *password, size_t password_len,
    const void *salt, size_t salt_len, uint32_t iterations,
    void *output, size_t output_len)
{
    br_hmac_key_context key;
    br_hmac_context hmac;
    unsigned char u[32], t[32], index[4], *dst = output;
    uint32_t block = 1;
    if (iterations == 0 || (!password && password_len) || (!salt && salt_len)
        || (!output && output_len))
        return 0;
#if SIZE_MAX > UINT32_MAX
    if (output_len > (size_t)UINT32_MAX * 32U) return 0;
#endif
    br_hmac_key_init(&key, &br_sha256_vtable, password, password_len);
    while (output_len) {
        size_t n = output_len < sizeof t ? output_len : sizeof t;
        index[0] = (unsigned char)(block >> 24);
        index[1] = (unsigned char)(block >> 16);
        index[2] = (unsigned char)(block >> 8);
        index[3] = (unsigned char)block;
        br_hmac_init(&hmac, &key, 0);
        br_hmac_update(&hmac, salt, salt_len);
        br_hmac_update(&hmac, index, sizeof index);
        br_hmac_out(&hmac, u);
        memcpy(t, u, sizeof t);
        for (uint32_t iteration = 1; iteration < iterations; ++iteration) {
            br_hmac_init(&hmac, &key, 0);
            br_hmac_update(&hmac, u, sizeof u);
            br_hmac_out(&hmac, u);
            for (size_t i = 0; i < sizeof t; ++i) t[i] ^= u[i];
        }
        memcpy(dst, t, n);
        dst += n;
        output_len -= n;
        ++block;
    }
    nm_crypto_zero(&key, sizeof key);
    nm_crypto_zero(&hmac, sizeof hmac);
    nm_crypto_zero(u, sizeof u);
    nm_crypto_zero(t, sizeof t);
    return 1;
}
