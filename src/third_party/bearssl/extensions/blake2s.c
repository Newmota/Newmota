/* RFC 7693 sequential BLAKE2s.
 * Copyright (C) 2026 Newmota contributors. SPDX-License-Identifier: GPL-3.0-or-later */
#include "newmota_crypto.h"
#include <string.h>

static const uint32_t iv[8] = {
    0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
    0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19
};
static const unsigned char sigma[10][16] = {
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
    {14,10,4,8,9,15,13,6,1,12,0,2,11,7,5,3},
    {11,8,12,0,5,2,15,13,10,14,3,6,7,1,9,4},
    {7,9,3,1,13,12,11,14,2,6,5,10,4,0,15,8},
    {9,0,5,7,2,4,10,15,14,1,11,12,6,8,3,13},
    {2,12,6,10,0,11,8,3,4,13,7,5,15,14,1,9},
    {12,5,1,15,14,13,4,10,0,7,6,3,9,2,8,11},
    {13,11,7,14,12,1,3,9,5,0,15,4,8,6,2,10},
    {6,15,14,9,11,3,0,8,12,2,13,7,1,4,10,5},
    {10,2,8,4,7,6,1,5,15,11,9,14,3,12,13,0}
};
static uint32_t read32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8
        | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t rotate(uint32_t x, unsigned n) { return x >> n | x << (32 - n); }
static void compress(nm_blake2s_context *ctx, int last) {
    uint32_t v[16], m[16];
    memcpy(v, ctx->h, sizeof ctx->h);
    memcpy(v + 8, iv, sizeof iv);
    v[12] ^= (uint32_t)ctx->count;
    v[13] ^= (uint32_t)(ctx->count >> 32);
    if (last) v[14] = ~v[14];
    for (unsigned i = 0; i < 16; ++i) m[i] = read32(ctx->buffer + i * 4);
#define G(a,b,c,d,x,y) do { \
    v[a] += v[b] + (x); v[d] = rotate(v[d] ^ v[a], 16); \
    v[c] += v[d]; v[b] = rotate(v[b] ^ v[c], 12); \
    v[a] += v[b] + (y); v[d] = rotate(v[d] ^ v[a], 8); \
    v[c] += v[d]; v[b] = rotate(v[b] ^ v[c], 7); \
} while (0)
    for (unsigned r = 0; r < 10; ++r) {
        const unsigned char *s = sigma[r];
        G(0,4,8,12,m[s[0]],m[s[1]]); G(1,5,9,13,m[s[2]],m[s[3]]);
        G(2,6,10,14,m[s[4]],m[s[5]]); G(3,7,11,15,m[s[6]],m[s[7]]);
        G(0,5,10,15,m[s[8]],m[s[9]]); G(1,6,11,12,m[s[10]],m[s[11]]);
        G(2,7,8,13,m[s[12]],m[s[13]]); G(3,4,9,14,m[s[14]],m[s[15]]);
    }
#undef G
    for (unsigned i = 0; i < 8; ++i) ctx->h[i] ^= v[i] ^ v[i + 8];
    nm_crypto_zero(v, sizeof v);
    nm_crypto_zero(m, sizeof m);
}
int nm_blake2s_init(nm_blake2s_context *ctx, size_t output_len,
    const void *key, size_t key_len)
{
    if (!ctx || output_len == 0 || output_len > 32 || key_len > 32
        || (!key && key_len)) return 0;
    memset(ctx, 0, sizeof *ctx);
    memcpy(ctx->h, iv, sizeof iv);
    ctx->h[0] ^= 0x01010000 | (uint32_t)key_len << 8 | (uint32_t)output_len;
    ctx->output_len = output_len;
    if (key_len) {
        memcpy(ctx->buffer, key, key_len);
        ctx->buffered = 64;
    }
    return 1;
}
int nm_blake2s_update(nm_blake2s_context *ctx, const void *data, size_t length) {
    const unsigned char *p = data;
    if (!ctx || ctx->output_len == 0 || ctx->output_len > 32
        || ctx->buffered > 64 || (!data && length)
        || ctx->count > UINT64_MAX - ctx->buffered
        || length > UINT64_MAX - ctx->count - ctx->buffered) return 0;
    while (length) {
        size_t n;
        if (ctx->buffered == 64) {
            ctx->count += 64;
            compress(ctx, 0);
            ctx->buffered = 0;
        }
        n = 64 - ctx->buffered;
        if (n > length) n = length;
        memcpy(ctx->buffer + ctx->buffered, p, n);
        ctx->buffered += n; p += n; length -= n;
    }
    return 1;
}
int nm_blake2s_out(const nm_blake2s_context *ctx, void *output) {
    nm_blake2s_context copy;
    unsigned char *p = output;
    if (!ctx || !output || ctx->output_len == 0 || ctx->output_len > 32
        || ctx->buffered > 64 || ctx->count > UINT64_MAX - ctx->buffered) return 0;
    copy = *ctx;
    copy.count += copy.buffered;
    memset(copy.buffer + copy.buffered, 0, 64 - copy.buffered);
    compress(&copy, 1);
    for (size_t i = 0; i < copy.output_len; ++i)
        p[i] = (unsigned char)(copy.h[i / 4] >> (8 * (i % 4)));
    nm_crypto_zero(&copy, sizeof copy);
    return 1;
}
int nm_hmac_blake2s(const void *key, size_t key_len,
    const void *data, size_t length, void *output)
{
    nm_blake2s_context ctx;
    unsigned char k[64] = {0}, digest[32];
    int ok = 0;
    if ((!key && key_len) || (!data && length) || !output) return 0;
    if (key_len > sizeof k) {
        nm_blake2s_init(&ctx, 32, NULL, 0);
        if (!nm_blake2s_update(&ctx, key, key_len)) goto cleanup;
        nm_blake2s_out(&ctx, k);
    } else if (key_len) memcpy(k, key, key_len);
    for (unsigned i = 0; i < sizeof k; ++i) k[i] ^= 0x36;
    nm_blake2s_init(&ctx, 32, NULL, 0);
    nm_blake2s_update(&ctx, k, sizeof k);
    if (!nm_blake2s_update(&ctx, data, length)) goto cleanup;
    nm_blake2s_out(&ctx, digest);
    for (unsigned i = 0; i < sizeof k; ++i) k[i] ^= 0x36 ^ 0x5c;
    nm_blake2s_init(&ctx, 32, NULL, 0);
    nm_blake2s_update(&ctx, k, sizeof k);
    nm_blake2s_update(&ctx, digest, sizeof digest);
    nm_blake2s_out(&ctx, output);
    ok = 1;
cleanup:
    nm_crypto_zero(k, sizeof k);
    nm_crypto_zero(digest, sizeof digest);
    nm_crypto_zero(&ctx, sizeof ctx);
    return ok;
}
