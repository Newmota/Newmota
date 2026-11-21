/*
  crypto_p256.c - Complete BearSSL P-256 point provider using ESP ECC hardware

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file
 * @brief BearSSL P-256 multiplication and muladd with no software point arithmetic.
 * Extended ECC hardware performs point validation, multiplication, addition and
 * all modular operations needed to normalize Jacobian addition results. Public
 * encodings, scalar validation and the BearSSL vtable remain unchanged. Other
 * curves and ECDSA's surrounding hash/signature flow keep their selected providers.
 */
#include "crypto_p256_glue.h"
#include "esp_crypto_lock.h"
#include "esp_crypto_periph_clk.h"
#include "esp_timer.h"
#include "hal/ecc_hal.h"
#include "hal/ecc_ll.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include <stdlib.h>

#if !SOC_ECC_SUPPORTED || !SOC_ECC_EXTENDED_MODES_SUPPORTED
#error "Complete BearSSL P-256 requires ECC point addition and field modular operations"
#endif
#if CONFIG_IDF_TARGET_ESP32H2
#error "ESP32-H2 before revision 1.2 ignores constant-time point multiplication"
#endif

/** @brief Fixed-size little-endian operands consumed by the ECC HAL. */
typedef struct {
    unsigned char x[32];
    unsigned char y[32];
    unsigned char z[32];
} point;

static unsigned nonzero(const unsigned char* a) {
    unsigned value = 0;
    for (size_t i = 0; i < 32; ++i)
        value |= a[i];
    return value != 0;
}

static unsigned equal(const unsigned char* a, const unsigned char* b) {
    unsigned diff = 0;
    for (size_t i = 0; i < 32; ++i)
        diff |= a[i] ^ b[i];
    return diff == 0;
}

static void reverse(unsigned char* dst, const unsigned char* src) {
    for (size_t i = 0; i < 32; ++i)
        dst[i] = src[31 - i];
}

/** @brief Big-endian scalar to the HAL's little-endian order, in place. */
static void reverse_in_place(unsigned char* a) {
    for (size_t i = 0; i < 16; ++i) {
        unsigned char t = a[i];
        a[i] = a[31 - i];
        a[31 - i] = t;
    }
}

static void decode_point(point* dst, const unsigned char* src) {
    reverse(dst->x, src + 1);
    reverse(dst->y, src + 33);
    memset(dst->z, 0, sizeof dst->z);
    dst->z[0] = 1;
}

static void encode_point(unsigned char* dst, const point* src) {
    dst[0] = 4;
    reverse(dst + 1, src->x);
    reverse(dst + 33, src->y);
}

/** @brief A peripheral timeout is fatal; BearSSL must never consume a substitute point. */
static void run(ecc_mode_t mode) {
    ecc_hal_set_mode(mode);
    ecc_hal_start_calc();
    const int64_t deadline = esp_timer_get_time() + 1000000;
    while (!ecc_hal_is_calc_finished()) {
        /* Re-read after the deadline: preemption alone must not look like a stall. */
        if (esp_timer_get_time() >= deadline && !ecc_hal_is_calc_finished())
            abort();
    }
}

/** @brief Execute complete scalar multiplication and on-curve validation. Caller owns the SDK lock.
 */
static unsigned multiply(point* p, const unsigned char* scalar) {
    ecc_hal_write_mul_param(scalar, p->x, p->y, 32);
    ecc_hal_enable_constant_time_point_mul(true);
    run(ECC_MODE_VERIFY_THEN_POINT_MUL);
    return ecc_hal_read_mul_result(p->x, p->y, 32) == 0;
}

/** @brief Compute a field product or quotient completely in hardware. */
static void field_operation(
    ecc_mode_t mode, const unsigned char* a, const unsigned char* b, unsigned char* out) {
    ecc_hal_write_mod_op_param(a, b, 32);
    ecc_hal_set_mod_base(ECC_MOD_P);
    run(mode);
    /* IDF's LL reports inverse mode as hardware value 11, not ECC_MODE_INVERSE_MUL (1),
       so the HAL's mode-based reader rejects it. Both products and quotients finish in PY. */
    if (mode == ECC_MODE_INVERSE_MUL)
        ecc_ll_read_param(ECC_PARAM_PY, out, 32);
    else if (ecc_hal_read_mod_op_result(out, 32) != 0)
        abort();
}

/** @brief Add affine points, detect infinity, and normalize using hardware field operations. */
static unsigned add(point* p, const point* q) {
    /* The hardware's mixed Jacobian addition yields Z = 0 both for P = -Q (infinity) and for
       P = Q. Double equal points by hardware multiplication by two instead. */
    if (equal(p->x, q->x) & equal(p->y, q->y)) {
        static const unsigned char two[32] = {2};
        return multiply(p, two);
    }
    unsigned char z2[32], z3[32];
    ecc_hal_write_point_add_param(p->x, p->y, q->x, q->y, q->z, 32);
    run(ECC_MODE_POINT_ADD);
    if (ecc_hal_read_point_add_result(p->x, p->y, p->z, 32, true) != 0)
        abort();
    if (!nonzero(p->z))
        return 0;
    field_operation(ECC_MODE_MOD_MUL, p->z, p->z, z2);
    field_operation(ECC_MODE_MOD_MUL, z2, p->z, z3);
    field_operation(ECC_MODE_INVERSE_MUL, p->x, z2, p->x);
    field_operation(ECC_MODE_INVERSE_MUL, p->y, z3, p->y);
    nm_crypto_zero(z2, sizeof z2);
    nm_crypto_zero(z3, sizeof z3);
    return 1;
}

/** @brief Own the SDK engine for the entire operation; contexts never retain engine state. */
static uint32_t calculate(unsigned char* a, const unsigned char* b, size_t len,
    const unsigned char* x, size_t xlen, const unsigned char* y, size_t ylen, int sum) {
    point p = {0}, q = {0};
    unsigned char k[32], l[32];
    const unsigned char* qb = b ? b : br_secp256r1.generator;
    uint32_t valid = 0;
    if (len != 65)
        return 0;
    if (!p256_canonical_point(a) || !p256_decode_scalar(k, x, xlen, sum))
        goto cleanup;
    if (sum && (!p256_canonical_point(qb) || !p256_decode_scalar(l, y, ylen, 1)))
        goto cleanup;
    /* A zero muladd term is infinity. Multiply by one so the point is still verified. */
    static const unsigned char one[32] = {1};
    const unsigned kz = !nonzero(k), lz = sum && !nonzero(l);
    decode_point(&p, a);
    reverse_in_place(k);
    if (sum) {
        decode_point(&q, qb);
        reverse_in_place(l);
    }
    esp_crypto_ecc_lock_acquire();
    esp_crypto_ecc_enable_periph_clk(true);
    valid = multiply(&p, kz ? one : k);
    if (sum) {
        valid &= multiply(&q, lz ? one : l);
        if (kz & lz)
            valid = 0;
        else if (kz)
            p = q;
        else if (valid && !lz)
            valid = add(&p, &q);
    }
    /* Clear all operand registers before releasing the SDK-owned engine. */
    unsigned char zero[32] = {0};
    ecc_hal_write_mul_param(zero, zero, zero, 32);
    ecc_hal_set_mod_base(ECC_MOD_N);
    esp_crypto_ecc_enable_periph_clk(false);
    esp_crypto_ecc_lock_release();
    if (valid)
        encode_point(a, &p);
cleanup:
    nm_crypto_zero(&p, sizeof p);
    nm_crypto_zero(&q, sizeof q);
    nm_crypto_zero(k, sizeof k);
    nm_crypto_zero(l, sizeof l);
    return valid;
}
