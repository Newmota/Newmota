/* Copyright (C) 2026 Newmota contributors. SPDX-License-Identifier: GPL-3.0-or-later */
#include "newmota_crypto.h"
#include "inner.h"

static uint32_t rotate(uint32_t x, unsigned n) { return x << n | x >> (32 - n); }
int nm_hchacha20(const void *key, const void *nonce, void *output) {
    const unsigned char *k = key, *n = nonce;
    unsigned char *out = output;
    uint32_t x[16] = {0x61707865,0x3320646e,0x79622d32,0x6b206574};
    if (!key || !nonce || !output) return 0;
    for (unsigned i = 0; i < 8; ++i) x[i + 4] = br_dec32le(k + i * 4);
    for (unsigned i = 0; i < 4; ++i) x[i + 12] = br_dec32le(n + i * 4);
#define Q(a,b,c,d) do { \
    x[a] += x[b]; x[d] = rotate(x[d] ^ x[a], 16); \
    x[c] += x[d]; x[b] = rotate(x[b] ^ x[c], 12); \
    x[a] += x[b]; x[d] = rotate(x[d] ^ x[a], 8); \
    x[c] += x[d]; x[b] = rotate(x[b] ^ x[c], 7); \
} while (0)
    for (unsigned i = 0; i < 10; ++i) {
        Q(0,4,8,12); Q(1,5,9,13); Q(2,6,10,14); Q(3,7,11,15);
        Q(0,5,10,15); Q(1,6,11,12); Q(2,7,8,13); Q(3,4,9,14);
    }
#undef Q
    for (unsigned i = 0; i < 4; ++i) {
        br_enc32le(out + i * 4, x[i]);
        br_enc32le(out + 16 + i * 4, x[i + 12]);
    }
    nm_crypto_zero(x, sizeof x);
    return 1;
}
/* Authentication-only callback: return the real block-0 MAC key, then leave
 * ciphertext unchanged. Avoids a payload-sized allocation during verification. */
static uint32_t authenticate(const void *key, const void *iv, uint32_t counter,
    void *data, size_t length)
{
    if (counter == 0) return br_chacha20_ct_run(key, iv, counter, data, length);
    return counter;
}
int nm_xchacha20_poly1305(const void *key, const void *nonce, void *data,
    size_t length, const void *aad, size_t aad_len, void *tag, int encrypt)
{
    unsigned char subkey[32], iv[12] = {0}, computed[16];
    int ok = 1;
    if (!key || !nonce || !tag || (!data && length) || (!aad && aad_len)
        || (uint64_t)length > UINT64_C(0xFFFFFFFF) * 64) return 0;
    nm_hchacha20(key, nonce, subkey);
    memcpy(iv + 4, (const unsigned char *)nonce + 16, 8);
    if (encrypt) {
        br_poly1305_ctmul_run(subkey, iv, data, length, aad, aad_len, tag,
            br_chacha20_ct_run, 1);
    } else {
        br_poly1305_ctmul_run(subkey, iv, data, length, aad, aad_len, computed,
            authenticate, 0);
        ok = nm_crypto_equal(computed, tag, sizeof computed);
        if (ok) br_chacha20_ct_run(subkey, iv, 1, data, length);
    }
    nm_crypto_zero(subkey, sizeof subkey);
    nm_crypto_zero(computed, sizeof computed);
    return ok;
}
