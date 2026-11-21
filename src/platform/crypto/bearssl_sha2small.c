/*
  bearssl_sha2small.c - BearSSL streaming state for hardware SHA compression

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/**
 * @file
 * @brief Adapted BearSSL streaming, padding and state serialization; runs of
 * complete blocks are dispatched to the selected native hardware provider.
 * Derived from the pinned import's hash/sha2small.c; upstream notice follows.
 */
/*
 * Copyright (c) 2016 Thomas Pornin <pornin@bolet.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "inner.h"
#include "newmota/platform/crypto_hash.h"

/* see inner.h */
const uint32_t br_sha224_IV[8] PROGMEM = {
    0xC1059ED8, 0x367CD507, 0x3070DD17, 0xF70E5939, 0xFFC00B31, 0x68581511, 0x64F98FA7, 0xBEFA4FA4};

/* see inner.h */
const uint32_t br_sha256_IV[8] PROGMEM = {
    0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A, 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19};

static void sha2small_update(br_sha224_context* cc, const void* data, size_t len) {
    const unsigned char* buf;
    size_t ptr;

    buf = data;
    ptr = (size_t)cc->count & 63;
    cc->count += (uint64_t)len;
    if (ptr > 0) {
        size_t clen;

        clen = 64 - ptr;
        if (clen > len) {
            clen = len;
        }
        memcpy_P(cc->buf + ptr, buf, clen);
        buf += clen;
        len -= clen;
        if (ptr + clen < 64) {
            return;
        }
        newmota_sha256_blocks(cc->buf, 1, cc->val);
    }
    /* Hand every complete input block to the provider in one run. */
    if (len >= 64) {
        newmota_sha256_blocks(buf, len >> 6, cc->val);
        buf += len & ~(size_t)63;
        len &= 63;
    }
    memcpy_P(cc->buf, buf, len);
}

static void sha2small_out(const br_sha224_context* cc, void* dst, int num) {
    unsigned char buf[128];
    uint32_t val[8];
    size_t ptr, end;

    /* Padding and length take one block, or two when fewer than 9 bytes remain. */
    ptr = (size_t)cc->count & 63;
    end = ptr < 56 ? 64 : 128;
    memcpy(buf, cc->buf, ptr);
    memcpy(val, cc->val, sizeof val);
    buf[ptr++] = 0x80;
    memset(buf + ptr, 0, end - 8 - ptr);
    br_enc64be(buf + end - 8, cc->count << 3);
    newmota_sha256_blocks(buf, end >> 6, val);
    br_range_enc32be(dst, val, num);
}

/* see bearssl.h */
void br_sha224_init(br_sha224_context* cc) {
    cc->vtable = &br_sha224_vtable;
    memcpy(cc->val, br_sha224_IV, sizeof cc->val);
    cc->count = 0;
}

/* see bearssl.h */
void br_sha224_update(br_sha224_context* cc, const void* data, size_t len) {
    sha2small_update(cc, data, len);
}

/* see bearssl.h */
void br_sha224_out(const br_sha224_context* cc, void* dst) {
    sha2small_out(cc, dst, 7);
}

/* see bearssl.h */
uint64_t br_sha224_state(const br_sha224_context* cc, void* dst) {
    br_range_enc32be(dst, cc->val, 8);
    return cc->count;
}

/* see bearssl.h */
void br_sha224_set_state(br_sha224_context* cc, const void* stb, uint64_t count) {
    br_range_dec32be(cc->val, 8, stb);
    cc->count = count;
}

/* see bearssl.h */
void br_sha256_init(br_sha256_context* cc) {
    cc->vtable = &br_sha256_vtable;
    memcpy(cc->val, br_sha256_IV, sizeof cc->val);
    cc->count = 0;
}

/* see bearssl.h */
void br_sha256_out(const br_sha256_context* cc, void* dst) {
    sha2small_out(cc, dst, 8);
}

/* see bearssl.h */
const br_hash_class br_sha224_vtable PROGMEM = {sizeof(br_sha224_context),
    BR_HASHDESC_ID(br_sha224_ID) | BR_HASHDESC_OUT(28) | BR_HASHDESC_STATE(32) |
        BR_HASHDESC_LBLEN(6) | BR_HASHDESC_MD_PADDING | BR_HASHDESC_MD_PADDING_BE,
    (void (*)(const br_hash_class**))&br_sha224_init,
    (void (*)(const br_hash_class**, const void*, size_t))&br_sha224_update,
    (void (*)(const br_hash_class* const*, void*))&br_sha224_out,
    (uint64_t (*)(const br_hash_class* const*, void*))&br_sha224_state,
    (void (*)(const br_hash_class**, const void*, uint64_t))&br_sha224_set_state};

/* see bearssl.h */
const br_hash_class br_sha256_vtable PROGMEM = {sizeof(br_sha256_context),
    BR_HASHDESC_ID(br_sha256_ID) | BR_HASHDESC_OUT(32) | BR_HASHDESC_STATE(32) |
        BR_HASHDESC_LBLEN(6) | BR_HASHDESC_MD_PADDING | BR_HASHDESC_MD_PADDING_BE,
    (void (*)(const br_hash_class**))&br_sha256_init,
    (void (*)(const br_hash_class**, const void*, size_t))&br_sha256_update,
    (void (*)(const br_hash_class* const*, void*))&br_sha256_out,
    (uint64_t (*)(const br_hash_class* const*, void*))&br_sha256_state,
    (void (*)(const br_hash_class**, const void*, uint64_t))&br_sha256_set_state};
