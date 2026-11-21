/*
  bearssl_sha2big.c - BearSSL streaming state for hardware SHA compression

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
 * Derived from the pinned import's hash/sha2big.c; upstream notice follows.
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

static const uint64_t IV384[8] PROGMEM = {0xCBBB9D5DC1059ED8, 0x629A292A367CD507,
    0x9159015A3070DD17, 0x152FECD8F70E5939, 0x67332667FFC00B31, 0x8EB44A8768581511,
    0xDB0C2E0D64F98FA7, 0x47B5481DBEFA4FA4};

static const uint64_t IV512[8] PROGMEM = {0x6A09E667F3BCC908, 0xBB67AE8584CAA73B,
    0x3C6EF372FE94F82B, 0xA54FF53A5F1D36F1, 0x510E527FADE682D1, 0x9B05688C2B3E6C1F,
    0x1F83D9ABFB41BD6B, 0x5BE0CD19137E2179};

static void sha2big_update(br_sha384_context* cc, const void* data, size_t len) {
    const unsigned char* buf;
    size_t ptr;

    buf = data;
    ptr = (size_t)cc->count & 127;
    cc->count += (uint64_t)len;
    if (ptr > 0) {
        size_t clen;

        clen = 128 - ptr;
        if (clen > len) {
            clen = len;
        }
        memcpy(cc->buf + ptr, buf, clen);
        buf += clen;
        len -= clen;
        if (ptr + clen < 128) {
            return;
        }
        newmota_sha512_blocks(cc->buf, 1, cc->val);
    }
    /* Hand every complete input block to the provider in one run. */
    if (len >= 128) {
        newmota_sha512_blocks(buf, len >> 7, cc->val);
        buf += len & ~(size_t)127;
        len &= 127;
    }
    memcpy(cc->buf, buf, len);
}

static void sha2big_out(const br_sha384_context* cc, void* dst, int num) {
    unsigned char buf[256];
    uint64_t val[8];
    size_t ptr, end;

    /* Padding and length take one block, or two when fewer than 17 bytes remain. */
    ptr = (size_t)cc->count & 127;
    end = ptr < 112 ? 128 : 256;
    memcpy(buf, cc->buf, ptr);
    memcpy(val, cc->val, sizeof val);
    buf[ptr++] = 0x80;
    memset(buf + ptr, 0, end - 16 - ptr);
    br_enc64be(buf + end - 16, cc->count >> 61);
    br_enc64be(buf + end - 8, cc->count << 3);
    newmota_sha512_blocks(buf, end >> 7, val);
    br_range_enc64be(dst, val, num);
}

/* see bearssl.h */
void br_sha384_init(br_sha384_context* cc) {
    cc->vtable = &br_sha384_vtable;
    memcpy(cc->val, IV384, sizeof IV384);
    cc->count = 0;
}

/* see bearssl.h */
void br_sha384_update(br_sha384_context* cc, const void* data, size_t len) {
    sha2big_update(cc, data, len);
}

/* see bearssl.h */
void br_sha384_out(const br_sha384_context* cc, void* dst) {
    sha2big_out(cc, dst, 6);
}

/* see bearssl.h */
uint64_t br_sha384_state(const br_sha384_context* cc, void* dst) {
    br_range_enc64be(dst, cc->val, 8);
    return cc->count;
}

/* see bearssl.h */
void br_sha384_set_state(br_sha384_context* cc, const void* stb, uint64_t count) {
    br_range_dec64be(cc->val, 8, stb);
    cc->count = count;
}

/* see bearssl.h */
void br_sha512_init(br_sha512_context* cc) {
    cc->vtable = &br_sha512_vtable;
    memcpy(cc->val, IV512, sizeof IV512);
    cc->count = 0;
}

/* see bearssl.h */
void br_sha512_out(const br_sha512_context* cc, void* dst) {
    sha2big_out(cc, dst, 8);
}

/* see bearssl.h */
const br_hash_class br_sha384_vtable PROGMEM = {sizeof(br_sha384_context),
    BR_HASHDESC_ID(br_sha384_ID) | BR_HASHDESC_OUT(48) | BR_HASHDESC_STATE(64) |
        BR_HASHDESC_LBLEN(7) | BR_HASHDESC_MD_PADDING | BR_HASHDESC_MD_PADDING_BE |
        BR_HASHDESC_MD_PADDING_128,
    (void (*)(const br_hash_class**))&br_sha384_init,
    (void (*)(const br_hash_class**, const void*, size_t))&br_sha384_update,
    (void (*)(const br_hash_class* const*, void*))&br_sha384_out,
    (uint64_t (*)(const br_hash_class* const*, void*))&br_sha384_state,
    (void (*)(const br_hash_class**, const void*, uint64_t))&br_sha384_set_state};

/* see bearssl.h */
const br_hash_class br_sha512_vtable PROGMEM = {sizeof(br_sha512_context),
    BR_HASHDESC_ID(br_sha512_ID) | BR_HASHDESC_OUT(64) | BR_HASHDESC_STATE(64) |
        BR_HASHDESC_LBLEN(7) | BR_HASHDESC_MD_PADDING | BR_HASHDESC_MD_PADDING_BE |
        BR_HASHDESC_MD_PADDING_128,
    (void (*)(const br_hash_class**))&br_sha512_init,
    (void (*)(const br_hash_class**, const void*, size_t))&br_sha512_update,
    (void (*)(const br_hash_class* const*, void*))&br_sha512_out,
    (uint64_t (*)(const br_hash_class* const*, void*))&br_sha512_state,
    (void (*)(const br_hash_class**, const void*, uint64_t))&br_sha512_set_state};
