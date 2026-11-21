/*
  bearssl_aes.c - BearSSL modes using complete hardware AES blocks

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file
 * @brief Preserve BearSSL AES mode semantics, context sizes, copies and vtables.
 * The opaque schedule storage holds only the raw key; num_rounds retains its usual
 * meaning. Every encryption/decryption and key expansion is done by hardware.
 * The software AES translation units are excluded together when this is selected.
 */
#include "inner.h"
#include "newmota/platform/crypto_aes.h"
#include "newmota_crypto.h"
#include <stdlib.h>

static unsigned init_key(void* dst, size_t capacity, const void* key, size_t len) {
    /* The hardware takes only these lengths; stop at the caller, not at the first block. */
    if (len != 16 && len != 24 && len != 32)
        abort();
    memset(dst, 0, capacity);
    memcpy(dst, key, len);
    return (unsigned)(len / 4 + 6);
}

static void xor_bytes(unsigned char* dst, const unsigned char* src, size_t len) {
    for (size_t i = 0; i < len; ++i)
        dst[i] ^= src[i];
}

static void blocks(
    const void* key, unsigned rounds, int decrypt, const void* in, void* out, size_t num) {
    newmota_aes_blocks(key, (rounds - 6) * 4, decrypt, in, out, num);
}

/** @brief CBC; decryption hands runs of blocks to the engine, encryption chains one by one. */
static void cbc(const void* key, unsigned rounds, int decrypt, void* iv, void* data, size_t len) {
    unsigned char* buf = data;
    unsigned char saved[NEWMOTA_AES_RUN_BLOCKS * 16];
    if (len % 16)
        abort();
    if (!decrypt) {
        for (; len; buf += 16, len -= 16) {
            xor_bytes(buf, iv, 16);
            blocks(key, rounds, 0, buf, buf, 1);
            memcpy(iv, buf, 16);
        }
        return;
    }
    while (len) {
        size_t run = len < sizeof saved ? len : sizeof saved;
        memcpy(saved, buf, run);
        blocks(key, rounds, 1, buf, buf, run / 16);
        xor_bytes(buf, iv, 16);
        xor_bytes(buf + 16, saved, run - 16);
        memcpy(iv, saved + run - 16, 16);
        buf += run;
        len -= run;
    }
    nm_crypto_zero(saved, sizeof saved);
}

/**
 * @brief Run 32-bit counter CTR and return the next counter.
 * @param partial Nonzero if a trailing partial block advances the returned counter, as in the
 * big, small and ct software implementations; ct64 returns the count of whole blocks only.
 */
static uint32_t ctr32(const void* key, unsigned rounds, const void* iv, uint32_t cc, void* data,
    size_t len, int partial) {
    unsigned char stream[NEWMOTA_AES_RUN_BLOCKS * 16];
    unsigned char* buf = data;
    uint32_t next = cc + (uint32_t)(len >> 4) + (uint32_t)(partial && (len & 15));
    while (len) {
        size_t run = len < sizeof stream ? len : sizeof stream;
        size_t num = (run + 15) / 16;
        for (size_t i = 0; i < num; ++i) {
            memcpy(stream + i * 16, iv, 12);
            br_enc32be(stream + i * 16 + 12, cc++);
        }
        blocks(key, rounds, 0, stream, stream, num);
        xor_bytes(buf, stream, run);
        buf += run;
        len -= run;
    }
    nm_crypto_zero(stream, sizeof stream);
    return next;
}

static void ctr128(const void* key, unsigned rounds, void* ctr, void* data, size_t len) {
    unsigned char* counter = ctr;
    unsigned char* buf = data;
    unsigned char stream[NEWMOTA_AES_RUN_BLOCKS * 16];
    while (len) {
        size_t run = len < sizeof stream ? len : sizeof stream;
        size_t num = (run + 15) / 16;
        for (size_t i = 0; i < num; ++i) {
            memcpy(stream + i * 16, counter, 16);
            for (size_t j = 16; j > 0; --j) {
                if (++counter[j - 1])
                    break;
            }
        }
        blocks(key, rounds, 0, stream, stream, num);
        xor_bytes(buf, stream, run);
        buf += run;
        len -= run;
    }
    nm_crypto_zero(stream, sizeof stream);
}

static void mac(const void* key, unsigned rounds, void* cbcmac, const void* data, size_t len) {
    const unsigned char* buf = data;
    if (len % 16)
        abort();
    while (len) {
        xor_bytes(cbcmac, buf, 16);
        blocks(key, rounds, 0, cbcmac, cbcmac, 1);
        buf += 16;
        len -= 16;
    }
}

/** @brief Initialize the big cbcenc ABI with a hardware key. */
void br_aes_big_cbcenc_init(br_aes_big_cbcenc_keys* ctx, const void* key, size_t len) {
    ctx->vtable = &br_aes_big_cbcenc_vtable;
    ctx->num_rounds = init_key(ctx->skey, sizeof ctx->skey, key, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
void br_aes_big_cbcenc_run(const br_aes_big_cbcenc_keys* ctx, void* iv, void* data, size_t len) {
    cbc(ctx->skey, ctx->num_rounds, 0, iv, data, len);
}

const br_block_cbcenc_class br_aes_big_cbcenc_vtable PROGMEM = {sizeof(br_aes_big_cbcenc_keys), 16,
    4, (void (*)(const br_block_cbcenc_class**, const void*, size_t))&br_aes_big_cbcenc_init,
    (void (*)(const br_block_cbcenc_class* const*, void*, void*, size_t))&br_aes_big_cbcenc_run};

/** @brief Initialize the big cbcdec ABI with a hardware key. */
void br_aes_big_cbcdec_init(br_aes_big_cbcdec_keys* ctx, const void* key, size_t len) {
    ctx->vtable = &br_aes_big_cbcdec_vtable;
    ctx->num_rounds = init_key(ctx->skey, sizeof ctx->skey, key, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
void br_aes_big_cbcdec_run(const br_aes_big_cbcdec_keys* ctx, void* iv, void* data, size_t len) {
    cbc(ctx->skey, ctx->num_rounds, 1, iv, data, len);
}

const br_block_cbcdec_class br_aes_big_cbcdec_vtable PROGMEM = {sizeof(br_aes_big_cbcdec_keys), 16,
    4, (void (*)(const br_block_cbcdec_class**, const void*, size_t))&br_aes_big_cbcdec_init,
    (void (*)(const br_block_cbcdec_class* const*, void*, void*, size_t))&br_aes_big_cbcdec_run};

/** @brief Initialize the big ctr ABI with a hardware key. */
void br_aes_big_ctr_init(br_aes_big_ctr_keys* ctx, const void* key, size_t len) {
    ctx->vtable = &br_aes_big_ctr_vtable;
    ctx->num_rounds = init_key(ctx->skey, sizeof ctx->skey, key, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
uint32_t br_aes_big_ctr_run(
    const br_aes_big_ctr_keys* ctx, const void* iv, uint32_t cc, void* data, size_t len) {
    return ctr32(ctx->skey, ctx->num_rounds, iv, cc, data, len, 1);
}

const br_block_ctr_class br_aes_big_ctr_vtable PROGMEM = {sizeof(br_aes_big_ctr_keys), 16, 4,
    (void (*)(const br_block_ctr_class**, const void*, size_t))&br_aes_big_ctr_init,
    (uint32_t (*)(const br_block_ctr_class* const*, const void*, uint32_t, void*,
        size_t))&br_aes_big_ctr_run};

/** @brief Initialize the big ctrcbc ABI with a hardware key. */
void br_aes_big_ctrcbc_init(br_aes_big_ctrcbc_keys* ctx, const void* key, size_t len) {
    ctx->vtable = &br_aes_big_ctrcbc_vtable;
    ctx->num_rounds = init_key(ctx->skey, sizeof ctx->skey, key, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
void br_aes_big_ctrcbc_ctr(const br_aes_big_ctrcbc_keys* ctx, void* ctr, void* data, size_t len) {
    ctr128(ctx->skey, ctx->num_rounds, ctr, data, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
void br_aes_big_ctrcbc_mac(
    const br_aes_big_ctrcbc_keys* ctx, void* cbcmac, const void* data, size_t len) {
    mac(ctx->skey, ctx->num_rounds, cbcmac, data, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
void br_aes_big_ctrcbc_encrypt(
    const br_aes_big_ctrcbc_keys* ctx, void* ctr, void* cbcmac, void* data, size_t len) {
    if (len % 16)
        abort();
    br_aes_big_ctrcbc_ctr(ctx, ctr, data, len);
    br_aes_big_ctrcbc_mac(ctx, cbcmac, data, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
void br_aes_big_ctrcbc_decrypt(
    const br_aes_big_ctrcbc_keys* ctx, void* ctr, void* cbcmac, void* data, size_t len) {
    if (len % 16)
        abort();
    br_aes_big_ctrcbc_mac(ctx, cbcmac, data, len);
    br_aes_big_ctrcbc_ctr(ctx, ctr, data, len);
}

const br_block_ctrcbc_class br_aes_big_ctrcbc_vtable PROGMEM = {sizeof(br_aes_big_ctrcbc_keys), 16,
    4, (void (*)(const br_block_ctrcbc_class**, const void*, size_t))&br_aes_big_ctrcbc_init,
    (void (*)(const br_block_ctrcbc_class* const*, void*, void*, void*,
        size_t))&br_aes_big_ctrcbc_encrypt,
    (void (*)(const br_block_ctrcbc_class* const*, void*, void*, void*,
        size_t))&br_aes_big_ctrcbc_decrypt,
    (void (*)(const br_block_ctrcbc_class* const*, void*, void*, size_t))&br_aes_big_ctrcbc_ctr,
    (void (*)(
        const br_block_ctrcbc_class* const*, void*, const void*, size_t))&br_aes_big_ctrcbc_mac};

/** @brief Initialize the small cbcenc ABI with a hardware key. */
void br_aes_small_cbcenc_init(br_aes_small_cbcenc_keys* ctx, const void* key, size_t len) {
    ctx->vtable = &br_aes_small_cbcenc_vtable;
    ctx->num_rounds = init_key(ctx->skey, sizeof ctx->skey, key, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
void br_aes_small_cbcenc_run(
    const br_aes_small_cbcenc_keys* ctx, void* iv, void* data, size_t len) {
    cbc(ctx->skey, ctx->num_rounds, 0, iv, data, len);
}

const br_block_cbcenc_class br_aes_small_cbcenc_vtable PROGMEM = {sizeof(br_aes_small_cbcenc_keys),
    16, 4, (void (*)(const br_block_cbcenc_class**, const void*, size_t))&br_aes_small_cbcenc_init,
    (void (*)(const br_block_cbcenc_class* const*, void*, void*, size_t))&br_aes_small_cbcenc_run};

/** @brief Initialize the small cbcdec ABI with a hardware key. */
void br_aes_small_cbcdec_init(br_aes_small_cbcdec_keys* ctx, const void* key, size_t len) {
    ctx->vtable = &br_aes_small_cbcdec_vtable;
    ctx->num_rounds = init_key(ctx->skey, sizeof ctx->skey, key, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
void br_aes_small_cbcdec_run(
    const br_aes_small_cbcdec_keys* ctx, void* iv, void* data, size_t len) {
    cbc(ctx->skey, ctx->num_rounds, 1, iv, data, len);
}

const br_block_cbcdec_class br_aes_small_cbcdec_vtable PROGMEM = {sizeof(br_aes_small_cbcdec_keys),
    16, 4, (void (*)(const br_block_cbcdec_class**, const void*, size_t))&br_aes_small_cbcdec_init,
    (void (*)(const br_block_cbcdec_class* const*, void*, void*, size_t))&br_aes_small_cbcdec_run};

/** @brief Initialize the small ctr ABI with a hardware key. */
void br_aes_small_ctr_init(br_aes_small_ctr_keys* ctx, const void* key, size_t len) {
    ctx->vtable = &br_aes_small_ctr_vtable;
    ctx->num_rounds = init_key(ctx->skey, sizeof ctx->skey, key, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
uint32_t br_aes_small_ctr_run(
    const br_aes_small_ctr_keys* ctx, const void* iv, uint32_t cc, void* data, size_t len) {
    return ctr32(ctx->skey, ctx->num_rounds, iv, cc, data, len, 1);
}

const br_block_ctr_class br_aes_small_ctr_vtable PROGMEM = {sizeof(br_aes_small_ctr_keys), 16, 4,
    (void (*)(const br_block_ctr_class**, const void*, size_t))&br_aes_small_ctr_init,
    (uint32_t (*)(const br_block_ctr_class* const*, const void*, uint32_t, void*,
        size_t))&br_aes_small_ctr_run};

/** @brief Initialize the small ctrcbc ABI with a hardware key. */
void br_aes_small_ctrcbc_init(br_aes_small_ctrcbc_keys* ctx, const void* key, size_t len) {
    ctx->vtable = &br_aes_small_ctrcbc_vtable;
    ctx->num_rounds = init_key(ctx->skey, sizeof ctx->skey, key, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
void br_aes_small_ctrcbc_ctr(
    const br_aes_small_ctrcbc_keys* ctx, void* ctr, void* data, size_t len) {
    ctr128(ctx->skey, ctx->num_rounds, ctr, data, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
void br_aes_small_ctrcbc_mac(
    const br_aes_small_ctrcbc_keys* ctx, void* cbcmac, const void* data, size_t len) {
    mac(ctx->skey, ctx->num_rounds, cbcmac, data, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
void br_aes_small_ctrcbc_encrypt(
    const br_aes_small_ctrcbc_keys* ctx, void* ctr, void* cbcmac, void* data, size_t len) {
    if (len % 16)
        abort();
    br_aes_small_ctrcbc_ctr(ctx, ctr, data, len);
    br_aes_small_ctrcbc_mac(ctx, cbcmac, data, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
void br_aes_small_ctrcbc_decrypt(
    const br_aes_small_ctrcbc_keys* ctx, void* ctr, void* cbcmac, void* data, size_t len) {
    if (len % 16)
        abort();
    br_aes_small_ctrcbc_mac(ctx, cbcmac, data, len);
    br_aes_small_ctrcbc_ctr(ctx, ctr, data, len);
}

const br_block_ctrcbc_class br_aes_small_ctrcbc_vtable PROGMEM = {sizeof(br_aes_small_ctrcbc_keys),
    16, 4, (void (*)(const br_block_ctrcbc_class**, const void*, size_t))&br_aes_small_ctrcbc_init,
    (void (*)(const br_block_ctrcbc_class* const*, void*, void*, void*,
        size_t))&br_aes_small_ctrcbc_encrypt,
    (void (*)(const br_block_ctrcbc_class* const*, void*, void*, void*,
        size_t))&br_aes_small_ctrcbc_decrypt,
    (void (*)(const br_block_ctrcbc_class* const*, void*, void*, size_t))&br_aes_small_ctrcbc_ctr,
    (void (*)(
        const br_block_ctrcbc_class* const*, void*, const void*, size_t))&br_aes_small_ctrcbc_mac};

/** @brief Initialize the ct cbcenc ABI with a hardware key. */
void br_aes_ct_cbcenc_init(br_aes_ct_cbcenc_keys* ctx, const void* key, size_t len) {
    ctx->vtable = &br_aes_ct_cbcenc_vtable;
    ctx->num_rounds = init_key(ctx->skey, sizeof ctx->skey, key, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
void br_aes_ct_cbcenc_run(const br_aes_ct_cbcenc_keys* ctx, void* iv, void* data, size_t len) {
    cbc(ctx->skey, ctx->num_rounds, 0, iv, data, len);
}

const br_block_cbcenc_class br_aes_ct_cbcenc_vtable PROGMEM = {sizeof(br_aes_ct_cbcenc_keys), 16, 4,
    (void (*)(const br_block_cbcenc_class**, const void*, size_t))&br_aes_ct_cbcenc_init,
    (void (*)(const br_block_cbcenc_class* const*, void*, void*, size_t))&br_aes_ct_cbcenc_run};

/** @brief Initialize the ct cbcdec ABI with a hardware key. */
void br_aes_ct_cbcdec_init(br_aes_ct_cbcdec_keys* ctx, const void* key, size_t len) {
    ctx->vtable = &br_aes_ct_cbcdec_vtable;
    ctx->num_rounds = init_key(ctx->skey, sizeof ctx->skey, key, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
void br_aes_ct_cbcdec_run(const br_aes_ct_cbcdec_keys* ctx, void* iv, void* data, size_t len) {
    cbc(ctx->skey, ctx->num_rounds, 1, iv, data, len);
}

const br_block_cbcdec_class br_aes_ct_cbcdec_vtable PROGMEM = {sizeof(br_aes_ct_cbcdec_keys), 16, 4,
    (void (*)(const br_block_cbcdec_class**, const void*, size_t))&br_aes_ct_cbcdec_init,
    (void (*)(const br_block_cbcdec_class* const*, void*, void*, size_t))&br_aes_ct_cbcdec_run};

/** @brief Initialize the ct ctr ABI with a hardware key. */
void br_aes_ct_ctr_init(br_aes_ct_ctr_keys* ctx, const void* key, size_t len) {
    ctx->vtable = &br_aes_ct_ctr_vtable;
    ctx->num_rounds = init_key(ctx->skey, sizeof ctx->skey, key, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
uint32_t br_aes_ct_ctr_run(
    const br_aes_ct_ctr_keys* ctx, const void* iv, uint32_t cc, void* data, size_t len) {
    return ctr32(ctx->skey, ctx->num_rounds, iv, cc, data, len, 1);
}

const br_block_ctr_class br_aes_ct_ctr_vtable PROGMEM = {sizeof(br_aes_ct_ctr_keys), 16, 4,
    (void (*)(const br_block_ctr_class**, const void*, size_t))&br_aes_ct_ctr_init,
    (uint32_t (*)(
        const br_block_ctr_class* const*, const void*, uint32_t, void*, size_t))&br_aes_ct_ctr_run};

/** @brief Initialize the ct ctrcbc ABI with a hardware key. */
void br_aes_ct_ctrcbc_init(br_aes_ct_ctrcbc_keys* ctx, const void* key, size_t len) {
    ctx->vtable = &br_aes_ct_ctrcbc_vtable;
    ctx->num_rounds = init_key(ctx->skey, sizeof ctx->skey, key, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
void br_aes_ct_ctrcbc_ctr(const br_aes_ct_ctrcbc_keys* ctx, void* ctr, void* data, size_t len) {
    ctr128(ctx->skey, ctx->num_rounds, ctr, data, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
void br_aes_ct_ctrcbc_mac(
    const br_aes_ct_ctrcbc_keys* ctx, void* cbcmac, const void* data, size_t len) {
    mac(ctx->skey, ctx->num_rounds, cbcmac, data, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
void br_aes_ct_ctrcbc_encrypt(
    const br_aes_ct_ctrcbc_keys* ctx, void* ctr, void* cbcmac, void* data, size_t len) {
    if (len % 16)
        abort();
    br_aes_ct_ctrcbc_ctr(ctx, ctr, data, len);
    br_aes_ct_ctrcbc_mac(ctx, cbcmac, data, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
void br_aes_ct_ctrcbc_decrypt(
    const br_aes_ct_ctrcbc_keys* ctx, void* ctr, void* cbcmac, void* data, size_t len) {
    if (len % 16)
        abort();
    br_aes_ct_ctrcbc_mac(ctx, cbcmac, data, len);
    br_aes_ct_ctrcbc_ctr(ctx, ctr, data, len);
}

const br_block_ctrcbc_class br_aes_ct_ctrcbc_vtable PROGMEM = {sizeof(br_aes_ct_ctrcbc_keys), 16, 4,
    (void (*)(const br_block_ctrcbc_class**, const void*, size_t))&br_aes_ct_ctrcbc_init,
    (void (*)(
        const br_block_ctrcbc_class* const*, void*, void*, void*, size_t))&br_aes_ct_ctrcbc_encrypt,
    (void (*)(
        const br_block_ctrcbc_class* const*, void*, void*, void*, size_t))&br_aes_ct_ctrcbc_decrypt,
    (void (*)(const br_block_ctrcbc_class* const*, void*, void*, size_t))&br_aes_ct_ctrcbc_ctr,
    (void (*)(
        const br_block_ctrcbc_class* const*, void*, const void*, size_t))&br_aes_ct_ctrcbc_mac};

/** @brief Initialize the ct64 cbcenc ABI with a hardware key. */
void br_aes_ct64_cbcenc_init(br_aes_ct64_cbcenc_keys* ctx, const void* key, size_t len) {
    ctx->vtable = &br_aes_ct64_cbcenc_vtable;
    ctx->num_rounds = init_key(ctx->skey, sizeof ctx->skey, key, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
void br_aes_ct64_cbcenc_run(const br_aes_ct64_cbcenc_keys* ctx, void* iv, void* data, size_t len) {
    cbc(ctx->skey, ctx->num_rounds, 0, iv, data, len);
}

const br_block_cbcenc_class br_aes_ct64_cbcenc_vtable PROGMEM = {sizeof(br_aes_ct64_cbcenc_keys),
    16, 4, (void (*)(const br_block_cbcenc_class**, const void*, size_t))&br_aes_ct64_cbcenc_init,
    (void (*)(const br_block_cbcenc_class* const*, void*, void*, size_t))&br_aes_ct64_cbcenc_run};

/** @brief Initialize the ct64 cbcdec ABI with a hardware key. */
void br_aes_ct64_cbcdec_init(br_aes_ct64_cbcdec_keys* ctx, const void* key, size_t len) {
    ctx->vtable = &br_aes_ct64_cbcdec_vtable;
    ctx->num_rounds = init_key(ctx->skey, sizeof ctx->skey, key, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
void br_aes_ct64_cbcdec_run(const br_aes_ct64_cbcdec_keys* ctx, void* iv, void* data, size_t len) {
    cbc(ctx->skey, ctx->num_rounds, 1, iv, data, len);
}

const br_block_cbcdec_class br_aes_ct64_cbcdec_vtable PROGMEM = {sizeof(br_aes_ct64_cbcdec_keys),
    16, 4, (void (*)(const br_block_cbcdec_class**, const void*, size_t))&br_aes_ct64_cbcdec_init,
    (void (*)(const br_block_cbcdec_class* const*, void*, void*, size_t))&br_aes_ct64_cbcdec_run};

/** @brief Initialize the ct64 ctr ABI with a hardware key. */
void br_aes_ct64_ctr_init(br_aes_ct64_ctr_keys* ctx, const void* key, size_t len) {
    ctx->vtable = &br_aes_ct64_ctr_vtable;
    ctx->num_rounds = init_key(ctx->skey, sizeof ctx->skey, key, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
uint32_t br_aes_ct64_ctr_run(
    const br_aes_ct64_ctr_keys* ctx, const void* iv, uint32_t cc, void* data, size_t len) {
    return ctr32(ctx->skey, ctx->num_rounds, iv, cc, data, len, 0);
}

const br_block_ctr_class br_aes_ct64_ctr_vtable PROGMEM = {sizeof(br_aes_ct64_ctr_keys), 16, 4,
    (void (*)(const br_block_ctr_class**, const void*, size_t))&br_aes_ct64_ctr_init,
    (uint32_t (*)(const br_block_ctr_class* const*, const void*, uint32_t, void*,
        size_t))&br_aes_ct64_ctr_run};

/** @brief Initialize the ct64 ctrcbc ABI with a hardware key. */
void br_aes_ct64_ctrcbc_init(br_aes_ct64_ctrcbc_keys* ctx, const void* key, size_t len) {
    ctx->vtable = &br_aes_ct64_ctrcbc_vtable;
    ctx->num_rounds = init_key(ctx->skey, sizeof ctx->skey, key, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
void br_aes_ct64_ctrcbc_ctr(const br_aes_ct64_ctrcbc_keys* ctx, void* ctr, void* data, size_t len) {
    ctr128(ctx->skey, ctx->num_rounds, ctr, data, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
void br_aes_ct64_ctrcbc_mac(
    const br_aes_ct64_ctrcbc_keys* ctx, void* cbcmac, const void* data, size_t len) {
    mac(ctx->skey, ctx->num_rounds, cbcmac, data, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
void br_aes_ct64_ctrcbc_encrypt(
    const br_aes_ct64_ctrcbc_keys* ctx, void* ctr, void* cbcmac, void* data, size_t len) {
    if (len % 16)
        abort();
    br_aes_ct64_ctrcbc_ctr(ctx, ctr, data, len);
    br_aes_ct64_ctrcbc_mac(ctx, cbcmac, data, len);
}

/** @brief Implement the BearSSL mode contract with hardware block operations. */
void br_aes_ct64_ctrcbc_decrypt(
    const br_aes_ct64_ctrcbc_keys* ctx, void* ctr, void* cbcmac, void* data, size_t len) {
    if (len % 16)
        abort();
    br_aes_ct64_ctrcbc_mac(ctx, cbcmac, data, len);
    br_aes_ct64_ctrcbc_ctr(ctx, ctr, data, len);
}

const br_block_ctrcbc_class br_aes_ct64_ctrcbc_vtable PROGMEM = {sizeof(br_aes_ct64_ctrcbc_keys),
    16, 4, (void (*)(const br_block_ctrcbc_class**, const void*, size_t))&br_aes_ct64_ctrcbc_init,
    (void (*)(const br_block_ctrcbc_class* const*, void*, void*, void*,
        size_t))&br_aes_ct64_ctrcbc_encrypt,
    (void (*)(const br_block_ctrcbc_class* const*, void*, void*, void*,
        size_t))&br_aes_ct64_ctrcbc_decrypt,
    (void (*)(const br_block_ctrcbc_class* const*, void*, void*, size_t))&br_aes_ct64_ctrcbc_ctr,
    (void (*)(
        const br_block_ctrcbc_class* const*, void*, const void*, size_t))&br_aes_ct64_ctrcbc_mac};
