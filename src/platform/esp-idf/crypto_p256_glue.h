/*
  crypto_p256_glue.h - Shared P-256 contract and BearSSL vtable for the ESP providers

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file
 * @brief Encoding checks and BearSSL glue shared by the ECC and MPI P-256 providers.
 * Included once by the selected provider, which defines calculate(); one provider is linked.
 */
#pragma once

#include "inner.h"
#include "newmota_crypto.h"

/**
 * @brief Provider backend: a = x*a, or a = x*a + y*b with b = NULL for the generator.
 * @return 1 and the encoded result in @p a, or 0 for an invalid input or an infinite result.
 */
static uint32_t calculate(unsigned char* a, const unsigned char* b, size_t len,
    const unsigned char* x, size_t xlen, const unsigned char* y, size_t ylen, int sum);

/** @brief Compare unsigned big-endian 256-bit encodings without early exits. */
static uint32_t p256_less_than(const unsigned char* a, const unsigned char* b) {
    uint32_t borrow = 0;
    for (size_t i = 32; i > 0; --i)
        borrow = ((uint32_t)a[i - 1] - b[i - 1] - borrow) >> 31;
    return borrow;
}

/**
 * @brief Decode a big-endian scalar below n, allowing any number of leading zero bytes.
 * Zero is accepted only with @p allow_zero, as BearSSL's muladd does for x and y.
 */
static uint32_t p256_decode_scalar(
    unsigned char* k, const unsigned char* src, size_t len, uint32_t allow_zero) {
    unsigned overflow = 0, nonzero = 0;
    memset(k, 0, 32);
    while (len > 32) {
        overflow |= *src++;
        --len;
    }
    memcpy(k + 32 - len, src, len);
    for (size_t i = 0; i < 32; ++i)
        nonzero |= k[i];
    return (overflow == 0) & (allow_zero | (nonzero != 0)) & p256_less_than(k, br_secp256r1.order);
}

/** @brief Accept only an uncompressed encoding with canonical coordinates; curve checks follow. */
static uint32_t p256_canonical_point(const unsigned char* src) {
    static const unsigned char p_be[32] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    return (src[0] == 4) & p256_less_than(src + 1, p_be) & p256_less_than(src + 33, p_be);
}

static const unsigned char* generator(int curve, size_t* len) {
    (void)curve;
    *len = br_secp256r1.generator_len;
    return br_secp256r1.generator;
}

static const unsigned char* order(int curve, size_t* len) {
    (void)curve;
    *len = br_secp256r1.order_len;
    return br_secp256r1.order;
}

static size_t xoff(int curve, size_t* len) {
    (void)curve;
    *len = 32;
    return 1;
}

static uint32_t mul(unsigned char* g, size_t len, const unsigned char* x, size_t xlen, int curve) {
    (void)curve;
    return calculate(g, NULL, len, x, xlen, NULL, 0, 0);
}

static size_t mulgen(unsigned char* r, const unsigned char* x, size_t xlen, int curve) {
    unsigned char result[65];
    memcpy(result, br_secp256r1.generator, sizeof result);
    if (!mul(result, sizeof result, x, xlen, curve))
        memset(result, 0, sizeof result);
    memcpy(r, result, sizeof result);
    nm_crypto_zero(result, sizeof result);
    return 65;
}

static uint32_t muladd(unsigned char* a, const unsigned char* b, size_t len, const unsigned char* x,
    size_t xlen, const unsigned char* y, size_t ylen, int curve) {
    (void)curve;
    return calculate(a, b, len, x, xlen, y, ylen, 1);
}

/** @brief Replacement for BearSSL's P-256 provider and its default dispatch. */
const br_ec_impl br_ec_p256_m15 PROGMEM = {
    (uint32_t)0x00800000, &generator, &order, &xoff, &mul, &mulgen, &muladd};
