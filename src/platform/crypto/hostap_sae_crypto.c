/*
  hostap_sae_crypto.c - hostap bignum and P-256 functions for WPA3-SAE on BearSSL

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file
 * @brief The crypto_bignum_* and crypto_ec_* functions of hostap's crypto.h that WPA3-SAE
 * (hunting-and-pecking and hash-to-element, group 19) calls. The ESP-IDF fork leaves them to
 * the application with CONFIG_ESP_WIFI_EXTERNAL_CRYPTO; hostap_symmetric_bearssl.c supplies
 * the supplicant's symmetric primitives.
 *
 * Bignums are unsigned with a fixed capacity of NM_BN_LIMBS 32-bit limbs (1024 bits), on
 * the heap as hostap expects. A result that would be negative or exceed the capacity fails
 * (-1) instead of wrapping, so the larger finite-field SAE groups fail at
 * crypto_bignum_init_set(); the supplicant allows only group 19 anyway.
 *
 * Modular arithmetic with an odd modulus (every SAE caller: the prime and the order) runs on
 * BearSSL's constant-time i15 code: reduction, addmod, mulmod, sqrmod, exptmod, legendre
 * and inverse take time independent of the operand values, which are password-derived in
 * hunting-and-pecking and hash-to-element. Only the operand byte lengths are visible, and
 * an operand longer than the modulus (the hash-to-field input) is public in length. The
 * exponent's bit length is visible too; SAE's group 19 exponents are public constants.
 * Exptmod, legendre and inverse need an odd modulus above 1, as mbedtls_mpi_exp_mod() does.
 * The rest (div, add, sub, rshift, cmp, mod and mulmod with an even modulus) is plain
 * variable-time limb arithmetic; division is Knuth's algorithm D.
 *
 * Points are affine P-256 coordinates with an infinity flag. Scalar multiplication and
 * point addition use br_ec_p256_m15, which is the hardware provider when USE_CRYPTO_HW_P256
 * is selected. Argument checks follow the replaced mbedTLS adapter: multiplication needs
 * 1 <= k < n and a valid point; addition needs two valid points and may return infinity.
 * Deviations, both failing closed: subtraction below zero fails; crypto_ec_point_to_bin()
 * refuses infinity. As in the ESP-IDF adapter, crypto_ec_get_a() returns a copy that the
 * caller frees (ESP-IDF's sswu() does); with NEWMOTA_HOSTAP_EC_A_BORROWED (Beken, whose
 * prebuilt upstream sae.o does not free it) it returns the curve's own value, as the other
 * getters always do.
 *
 * SAE-PK and OWE STA use the EC key and ECDH functions at the end, P-256 only:
 * crypto_ec_key_parse_pub() accepts exactly the DER SubjectPublicKeyInfo of a P-256 key
 * (uncompressed or compressed point) and rejects every other curve or encoding;
 * crypto_ec_key_verify_signature() is BearSSL's ECDSA verify on the same P-256 provider;
 * crypto_ecdh_*() generate the key with os_get_random() and take the peer's x coordinate,
 * as OWE sends it. The shared secret is the x coordinate.
 */
#include "utils/includes.h"
#include "utils/common.h"
#include "crypto/crypto.h"
#include "utils/wpabuf.h"

#include <stdint.h>
#include <string.h>

#include "bearssl_ec.h"
/* inner.h defines MIN and MAX as functions; the SDK's sys/param.h has them as macros. This
   file uses neither. */
#undef MIN
#undef MAX
#include "newmota/platform/bearssl_inner.h"
#include "newmota_crypto.h"

#define NM_BN_LIMBS 32
#define NM_BN_WIDE (2 * NM_BN_LIMBS)
#define NM_P256_LEN 32
/* An i15 integer for a modulus up to NM_BN_LIMBS * 32 bits: 69 15-bit words, the header
   word, and an even pad (br_i15_modpow_opt rounds the per-value stride up to even). */
#define NM_I15_WORDS 72

struct crypto_bignum {
    size_t n;                /* significant limbs; v[n - 1] != 0 unless n == 0 */
    uint32_t v[NM_BN_LIMBS]; /* little-endian limbs; limbs at and above n are zero */
};

struct crypto_ec {
    struct crypto_bignum prime;
    struct crypto_bignum order;
    struct crypto_bignum a;
    struct crypto_bignum b;
};

struct crypto_ec_point {
    int infinity;
    u8 xy[2 * NM_P256_LEN]; /* big-endian x || y, as received; validated before use */
};

/* ---- limb arithmetic ---------------------------------------------------------------- */

static int bn_set(struct crypto_bignum* x, const uint32_t* v, size_t n) {
    while (n > 0 && v[n - 1] == 0) {
        n--;
    }
    if (n > NM_BN_LIMBS) {
        return -1;
    }
    memmove(x->v, v, n * sizeof(uint32_t));
    memset(x->v + n, 0, (NM_BN_LIMBS - n) * sizeof(uint32_t));
    x->n = n;
    return 0;
}

static void bn_set_uint(struct crypto_bignum* x, uint32_t value) {
    memset(x, 0, sizeof(*x));
    x->v[0] = value;
    x->n = value != 0;
}

static int bn_copy(struct crypto_bignum* dst, const struct crypto_bignum* src) {
    if (dst != src) {
        memcpy(dst, src, sizeof(*dst));
    }
    return 0;
}

static int bn_from_be(struct crypto_bignum* x, const u8* buf, size_t len) {
    while (len > 0 && buf[0] == 0) {
        buf++;
        len--;
    }
    if (len > NM_BN_LIMBS * sizeof(uint32_t)) {
        return -1;
    }
    memset(x, 0, sizeof(*x));
    for (size_t i = 0; i < len; i++) {
        size_t bit = (len - 1 - i) * 8;
        x->v[bit / 32] |= (uint32_t)buf[i] << (bit % 32);
    }
    x->n = (len + 3) / 4;
    while (x->n > 0 && x->v[x->n - 1] == 0) {
        x->n--;
    }
    return 0;
}

static size_t bn_bits(const struct crypto_bignum* x) {
    return x->n ? 32 * (x->n - 1) + (32 - (size_t)__builtin_clz(x->v[x->n - 1])) : 0;
}

static int bn_cmp(const struct crypto_bignum* a, const struct crypto_bignum* b) {
    if (a->n != b->n) {
        return a->n > b->n ? 1 : -1;
    }
    for (size_t i = a->n; i-- > 0;) {
        if (a->v[i] != b->v[i]) {
            return a->v[i] > b->v[i] ? 1 : -1;
        }
    }
    return 0;
}

static int bn_is_one(const struct crypto_bignum* x) {
    return x->n == 1 && x->v[0] == 1;
}

static int bn_add(
    const struct crypto_bignum* a, const struct crypto_bignum* b, struct crypto_bignum* c) {
    uint32_t t[NM_BN_LIMBS + 1];
    size_t n = a->n > b->n ? a->n : b->n;
    uint64_t carry = 0;
    for (size_t i = 0; i < n; i++) {
        carry += (uint64_t)a->v[i] + b->v[i];
        t[i] = (uint32_t)carry;
        carry >>= 32;
    }
    t[n] = (uint32_t)carry;
    return bn_set(c, t, n + 1);
}

static int bn_sub(
    const struct crypto_bignum* a, const struct crypto_bignum* b, struct crypto_bignum* c) {
    uint32_t t[NM_BN_LIMBS];
    uint64_t borrow = 0;
    if (bn_cmp(a, b) < 0) {
        return -1;
    }
    for (size_t i = 0; i < a->n; i++) {
        uint64_t d = (uint64_t)a->v[i] - b->v[i] - borrow;
        t[i] = (uint32_t)d;
        borrow = d >> 63;
    }
    return bn_set(c, t, a->n);
}

static void bn_shr1(struct crypto_bignum* x) {
    for (size_t i = 0; i < x->n; i++) {
        x->v[i] = (x->v[i] >> 1) | (i + 1 < x->n ? x->v[i + 1] << 31 : 0);
    }
    if (x->n > 0 && x->v[x->n - 1] == 0) {
        x->n--;
    }
}

static void limbs_mul(const uint32_t* a, size_t an, const uint32_t* b, size_t bn, uint32_t* out) {
    memset(out, 0, (an + bn) * sizeof(uint32_t));
    for (size_t i = 0; i < an; i++) {
        uint64_t carry = 0;
        for (size_t j = 0; j < bn; j++) {
            carry += (uint64_t)a[i] * b[j] + out[i + j];
            out[i + j] = (uint32_t)carry;
            carry >>= 32;
        }
        out[i + bn] = (uint32_t)carry;
    }
}

/**
 * Knuth's algorithm D (Hacker's Delight divmnu64). u has m limbs, v has n limbs with
 * v[n - 1] != 0, n <= m <= NM_BN_WIDE, n <= NM_BN_LIMBS. q receives m - n + 1 limbs, r n limbs.
 */
static void limbs_divmod(
    const uint32_t* u, size_t m, const uint32_t* v, size_t n, uint32_t* q, uint32_t* r) {
    if (n == 1) {
        uint64_t k = 0;
        for (size_t j = m; j-- > 0;) {
            uint64_t t = (k << 32) | u[j];
            uint32_t qj = (uint32_t)(t / v[0]);
            q[j] = qj;
            k = t - (uint64_t)qj * v[0];
        }
        r[0] = (uint32_t)k;
        return;
    }
    uint32_t un[NM_BN_WIDE + 1];
    uint32_t vn[NM_BN_LIMBS];
    const unsigned s = (unsigned)__builtin_clz(v[n - 1]);
    for (size_t i = n - 1; i > 0; i--) {
        vn[i] = (v[i] << s) | (uint32_t)((uint64_t)v[i - 1] >> (32 - s));
    }
    vn[0] = v[0] << s;
    un[m] = (uint32_t)((uint64_t)u[m - 1] >> (32 - s));
    for (size_t i = m - 1; i > 0; i--) {
        un[i] = (u[i] << s) | (uint32_t)((uint64_t)u[i - 1] >> (32 - s));
    }
    un[0] = u[0] << s;
    for (size_t j = m - n + 1; j-- > 0;) {
        const uint64_t num = ((uint64_t)un[j + n] << 32) | un[j + n - 1];
        uint64_t qhat = num / vn[n - 1];
        uint64_t rhat = num - qhat * vn[n - 1];
        while (qhat >> 32 || qhat * vn[n - 2] > ((rhat << 32) | un[j + n - 2])) {
            qhat--;
            rhat += vn[n - 1];
            if (rhat >> 32) {
                break;
            }
        }
        int64_t k = 0;
        int64_t t;
        for (size_t i = 0; i < n; i++) {
            const uint64_t p = qhat * vn[i];
            t = (int64_t)un[i + j] - k - (int64_t)(p & 0xFFFFFFFFu);
            un[i + j] = (uint32_t)t;
            k = (int64_t)(p >> 32) - (t >> 32);
        }
        t = (int64_t)un[j + n] - k;
        un[j + n] = (uint32_t)t;
        if (t < 0) {
            uint64_t c = 0;
            qhat--;
            for (size_t i = 0; i < n; i++) {
                c += (uint64_t)un[i + j] + vn[i];
                un[i + j] = (uint32_t)c;
                c >>= 32;
            }
            un[j + n] += (uint32_t)c;
        }
        q[j] = (uint32_t)qhat;
    }
    for (size_t i = 0; i + 1 < n; i++) {
        r[i] = (un[i] >> s) | (uint32_t)((uint64_t)un[i + 1] << (32 - s));
    }
    r[n - 1] = un[n - 1] >> s;
    nm_crypto_zero(un, sizeof(un));
}

/** q = u / d, r = u % d for a limb array u of m limbs; q or r may be NULL. */
static int bn_divmod(const uint32_t* u, size_t m, const struct crypto_bignum* d,
    struct crypto_bignum* q, struct crypto_bignum* r) {
    uint32_t qq[NM_BN_WIDE + 1];
    uint32_t rr[NM_BN_LIMBS];
    int ret = 0;
    if (d->n == 0) {
        return -1;
    }
    while (m > 0 && u[m - 1] == 0) {
        m--;
    }
    if (m > NM_BN_WIDE) {
        return -1;
    }
    if (m < d->n) {
        if (r != NULL && bn_set(r, u, m) < 0) {
            return -1;
        }
        if (q != NULL) {
            bn_set_uint(q, 0);
        }
        return 0;
    }
    limbs_divmod(u, m, d->v, d->n, qq, rr);
    if (q != NULL) {
        ret |= bn_set(q, qq, m - d->n + 1);
    }
    if (r != NULL) {
        ret |= bn_set(r, rr, d->n);
    }
    nm_crypto_zero(qq, sizeof(qq));
    nm_crypto_zero(rr, sizeof(rr));
    return ret < 0 ? -1 : 0;
}

static int bn_mulmod(const struct crypto_bignum* a, const struct crypto_bignum* b,
    const struct crypto_bignum* m, struct crypto_bignum* d) {
    uint32_t t[NM_BN_WIDE];
    int ret;
    if (m->n == 0) {
        return -1;
    }
    limbs_mul(a->v, a->n, b->v, b->n, t);
    ret = bn_divmod(t, a->n + b->n, m, NULL, d);
    nm_crypto_zero(t, sizeof(t));
    return ret;
}

/* ---- constant-time arithmetic modulo an odd modulus (BearSSL i15) -------------------- */

struct nm_mod {
    uint16_t m[NM_I15_WORDS]; /* modulus, i15 encoding */
    uint16_t m0i;             /* -1/m mod 2^15 */
    size_t len;               /* modulus byte length */
};

/** The CT path applies to odd moduli above 1; anything else returns -1. */
static int nm_mod_init(struct nm_mod* md, const struct crypto_bignum* m) {
    u8 buf[NM_BN_LIMBS * sizeof(uint32_t)];
    if (m->n == 0 || !(m->v[0] & 1) || bn_is_one(m)) {
        return -1;
    }
    md->len = (bn_bits(m) + 7) / 8;
    crypto_bignum_to_bin(m, buf, sizeof(buf), 0);
    br_i15_decode(md->m, buf, md->len);
    md->m0i = br_i15_ninv15(md->m[1]);
    return 0;
}

static size_t nm_words(const struct nm_mod* md) {
    return ((size_t)(md->m[0] + 15) >> 4) + 1; /* value words and the header */
}

/** x = a mod m. Encodes a at the modulus length (or a's own, if longer) so the reduction's
    running time does not depend on a's value. */
static void nm_load(uint16_t* x, const struct nm_mod* md, const struct crypto_bignum* a) {
    u8 buf[NM_BN_LIMBS * sizeof(uint32_t)];
    size_t len = (bn_bits(a) + 7) / 8;
    if (len < md->len) {
        len = md->len;
    }
    crypto_bignum_to_bin(a, buf, sizeof(buf), len);
    br_i15_decode_reduce(x, buf, len, md->m);
    nm_crypto_zero(buf, len);
}

static int nm_store(struct crypto_bignum* d, const struct nm_mod* md, const uint16_t* x) {
    u8 buf[NM_BN_LIMBS * sizeof(uint32_t)];
    int ret;
    br_i15_encode(buf, md->len, x);
    ret = bn_from_be(d, buf, md->len);
    nm_crypto_zero(buf, md->len);
    return ret;
}

/** d = a * b mod m; d may alias a or b. */
static void nm_mul(uint16_t* d, const uint16_t* a, const uint16_t* b, const struct nm_mod* md) {
    uint16_t am[NM_I15_WORDS], t[NM_I15_WORDS];
    const size_t words = nm_words(md);
    memcpy(am, a, words * sizeof(uint16_t));
    br_i15_to_monty(am, md->m);
    br_i15_montymul(t, am, b, md->m, md->m0i); /* (a R) b / R */
    memcpy(d, t, words * sizeof(uint16_t));
    nm_crypto_zero(am, sizeof(am));
    nm_crypto_zero(t, sizeof(t));
}

/** a = a + b mod m, for a, b in [0, m). */
static void nm_add(uint16_t* a, const uint16_t* b, const struct nm_mod* md) {
    const uint32_t carry = br_i15_add(a, b, 1);
    br_i15_sub(a, md->m, carry | NOT(br_i15_sub(a, md->m, 0)));
}

static int ct_mod(const struct crypto_bignum* a, const struct nm_mod* md, struct crypto_bignum* d) {
    uint16_t x[NM_I15_WORDS];
    int ret;
    nm_load(x, md, a);
    ret = nm_store(d, md, x);
    nm_crypto_zero(x, sizeof(x));
    return ret;
}

static int ct_mulmod(const struct crypto_bignum* a, const struct crypto_bignum* b,
    const struct nm_mod* md, struct crypto_bignum* d) {
    uint16_t x[NM_I15_WORDS], y[NM_I15_WORDS];
    int ret;
    nm_load(x, md, a);
    nm_load(y, md, b);
    nm_mul(x, x, y, md);
    ret = nm_store(d, md, x);
    nm_crypto_zero(x, sizeof(x));
    nm_crypto_zero(y, sizeof(y));
    return ret;
}

static int ct_addmod(const struct crypto_bignum* a, const struct crypto_bignum* b,
    const struct nm_mod* md, struct crypto_bignum* d) {
    uint16_t x[NM_I15_WORDS], y[NM_I15_WORDS];
    int ret;
    nm_load(x, md, a);
    nm_load(y, md, b);
    nm_add(x, y, md);
    ret = nm_store(d, md, x);
    nm_crypto_zero(x, sizeof(x));
    nm_crypto_zero(y, sizeof(y));
    return ret;
}

/** x = x^e mod m in place, x in [0, m). Constant-time in x and in e's bits; e's byte length
    is visible. The window table is on the heap (680 bytes for P-256). */
static int nm_pow(uint16_t* x, const struct crypto_bignum* e, const struct nm_mod* md) {
    u8 ebuf[NM_BN_LIMBS * sizeof(uint32_t)];
    const size_t elen = (bn_bits(e) + 7) / 8;
    size_t stride = nm_words(md);
    size_t twlen;
    uint16_t* tmp;
    uint32_t ok;
    stride += stride & 1;
    twlen = 17 * stride; /* a 4-bit window: 2^4 + 1 values */
    tmp = os_malloc(twlen * sizeof(uint16_t));
    if (tmp == NULL) {
        return -1;
    }
    crypto_bignum_to_bin(e, ebuf, sizeof(ebuf), 0);
    ok = br_i15_modpow_opt(x, ebuf, elen, md->m, md->m0i, tmp, twlen);
    nm_crypto_zero(tmp, twlen * sizeof(uint16_t));
    os_free(tmp);
    nm_crypto_zero(ebuf, sizeof(ebuf));
    return ok ? 0 : -1;
}

/** out = x^3 + a x + b mod p for the curve; out may alias x. */
static int bn_y_sqr(
    const struct crypto_ec* e, const struct crypto_bignum* x, struct crypto_bignum* out) {
    struct nm_mod md;
    uint16_t xx[NM_I15_WORDS], t[NM_I15_WORDS], u[NM_I15_WORDS];
    int ret = -1;
    if (nm_mod_init(&md, &e->prime) == 0) {
        nm_load(xx, &md, x);
        nm_mul(t, xx, xx, &md);
        nm_mul(t, t, xx, &md);
        nm_load(u, &md, &e->a);
        nm_mul(u, u, xx, &md);
        nm_add(t, u, &md);
        nm_load(u, &md, &e->b);
        nm_add(t, u, &md);
        ret = nm_store(out, &md, t);
    }
    nm_crypto_zero(xx, sizeof(xx));
    nm_crypto_zero(t, sizeof(t));
    nm_crypto_zero(u, sizeof(u));
    return ret;
}

/* ---- hostap bignum API -------------------------------------------------------------- */

struct crypto_bignum* crypto_bignum_init(void) {
    return os_zalloc(sizeof(struct crypto_bignum));
}

struct crypto_bignum* crypto_bignum_init_set(const u8* buf, size_t len) {
    struct crypto_bignum* x = crypto_bignum_init();
    if (x != NULL && bn_from_be(x, buf, len) < 0) {
        os_free(x);
        x = NULL;
    }
    return x;
}

struct crypto_bignum* crypto_bignum_init_uint(unsigned int val) {
    struct crypto_bignum* x = crypto_bignum_init();
    if (x != NULL) {
        bn_set_uint(x, val);
    }
    return x;
}

void crypto_bignum_deinit(struct crypto_bignum* n, int clear) {
    (void)clear; /* always cleared, as the mbedTLS adapter did */
    if (n != NULL) {
        nm_crypto_zero(n, sizeof(*n));
        os_free(n);
    }
}

int crypto_bignum_to_bin(const struct crypto_bignum* a, u8* buf, size_t buflen, size_t padlen) {
    const size_t num_bytes = (bn_bits(a) + 7) / 8;
    size_t offset;
    if (padlen > buflen || num_bytes > buflen) {
        return -1;
    }
    offset = padlen > num_bytes ? padlen - num_bytes : 0;
    memset(buf, 0, offset);
    for (size_t i = 0; i < num_bytes; i++) {
        size_t bit = (num_bytes - 1 - i) * 8;
        buf[offset + i] = (u8)(a->v[bit / 32] >> (bit % 32));
    }
    return (int)(num_bytes + offset);
}

int crypto_bignum_rand(struct crypto_bignum* r, const struct crypto_bignum* m) {
    u8 buf[NM_BN_LIMBS * sizeof(uint32_t)];
    const size_t bits = bn_bits(m);
    const size_t len = (bits + 7) / 8;
    int ret = -1;
    if (bits == 0) {
        return -1;
    }
    /* Rejection sampling over [0, m), as mbedtls_mpi_random(r, 0, m). */
    for (int attempt = 0; attempt < 64; attempt++) {
        struct crypto_bignum t;
        if (os_get_random(buf, len) < 0) {
            break;
        }
        if (bits % 8) {
            buf[0] &= (u8)((1u << (bits % 8)) - 1);
        }
        if (bn_from_be(&t, buf, len) == 0 && bn_cmp(&t, m) < 0) {
            ret = bn_copy(r, &t);
            nm_crypto_zero(&t, sizeof(t));
            break;
        }
    }
    nm_crypto_zero(buf, sizeof(buf));
    return ret;
}

int crypto_bignum_add(
    const struct crypto_bignum* a, const struct crypto_bignum* b, struct crypto_bignum* c) {
    return bn_add(a, b, c);
}

int crypto_bignum_mod(
    const struct crypto_bignum* a, const struct crypto_bignum* b, struct crypto_bignum* c) {
    struct nm_mod md;
    if (nm_mod_init(&md, b) == 0) {
        return ct_mod(a, &md, c);
    }
    return bn_divmod(a->v, a->n, b, NULL, c);
}

int crypto_bignum_exptmod(const struct crypto_bignum* a, const struct crypto_bignum* b,
    const struct crypto_bignum* c, struct crypto_bignum* d) {
    struct nm_mod md;
    uint16_t x[NM_I15_WORDS];
    int ret = -1;
    if (nm_mod_init(&md, c) < 0) {
        return -1;
    }
    nm_load(x, &md, a);
    if (nm_pow(x, b, &md) == 0) {
        ret = nm_store(d, &md, x);
    }
    nm_crypto_zero(x, sizeof(x));
    return ret;
}

int crypto_bignum_inverse(
    const struct crypto_bignum* a, const struct crypto_bignum* b, struct crypto_bignum* c) {
    /* c = 1 / a mod b by BearSSL's constant-time binary GCD; fails if a is not invertible. */
    struct nm_mod md;
    uint16_t x[NM_I15_WORDS], y[NM_I15_WORDS], t[3 * NM_I15_WORDS];
    int ret = -1;
    if (nm_mod_init(&md, b) < 0) {
        return -1;
    }
    nm_load(y, &md, a);
    br_i15_zero(x, md.m[0]);
    x[1] = 1;
    if (br_i15_moddiv(x, y, md.m, md.m0i, t)) {
        ret = nm_store(c, &md, x);
    }
    nm_crypto_zero(x, sizeof(x));
    nm_crypto_zero(y, sizeof(y));
    nm_crypto_zero(t, sizeof(t));
    return ret;
}

int crypto_bignum_sub(
    const struct crypto_bignum* a, const struct crypto_bignum* b, struct crypto_bignum* c) {
    return bn_sub(a, b, c);
}

int crypto_bignum_div(
    const struct crypto_bignum* a, const struct crypto_bignum* b, struct crypto_bignum* c) {
    return bn_divmod(a->v, a->n, b, c, NULL);
}

int crypto_bignum_addmod(const struct crypto_bignum* a, const struct crypto_bignum* b,
    const struct crypto_bignum* c, struct crypto_bignum* d) {
    struct nm_mod md;
    struct crypto_bignum t;
    int ret = -1;
    if (nm_mod_init(&md, c) == 0) {
        return ct_addmod(a, b, &md, d);
    }
    if (bn_add(a, b, &t) == 0) {
        ret = bn_divmod(t.v, t.n, c, NULL, d);
    }
    nm_crypto_zero(&t, sizeof(t));
    return ret;
}

int crypto_bignum_mulmod(const struct crypto_bignum* a, const struct crypto_bignum* b,
    const struct crypto_bignum* c, struct crypto_bignum* d) {
    struct nm_mod md;
    if (nm_mod_init(&md, c) == 0) {
        return ct_mulmod(a, b, &md, d);
    }
    return bn_mulmod(a, b, c, d);
}

int crypto_bignum_sqrmod(
    const struct crypto_bignum* a, const struct crypto_bignum* b, struct crypto_bignum* c) {
    return crypto_bignum_mulmod(a, a, b, c);
}

int crypto_bignum_rshift(const struct crypto_bignum* a, int n, struct crypto_bignum* r) {
    uint32_t t[NM_BN_LIMBS] = {0};
    size_t words, bits;
    if (n < 0) {
        return -1;
    }
    words = (size_t)n / 32;
    bits = (size_t)n % 32;
    for (size_t i = 0; i + words < a->n; i++) {
        uint64_t hi = i + words + 1 < a->n ? a->v[i + words + 1] : 0;
        t[i] = (uint32_t)((((uint64_t)hi << 32) | a->v[i + words]) >> bits);
    }
    return bn_set(r, t, NM_BN_LIMBS);
}

int crypto_bignum_cmp(const struct crypto_bignum* a, const struct crypto_bignum* b) {
    return bn_cmp(a, b);
}

int crypto_bignum_bits(const struct crypto_bignum* a) {
    return (int)bn_bits(a);
}

int crypto_bignum_is_zero(const struct crypto_bignum* a) {
    return a->n == 0;
}

int crypto_bignum_is_one(const struct crypto_bignum* a) {
    return bn_is_one(a);
}

int crypto_bignum_is_odd(const struct crypto_bignum* a) {
    return a->n > 0 && (a->v[0] & 1);
}

int crypto_bignum_legendre(const struct crypto_bignum* a, const struct crypto_bignum* p) {
    /* a^((p - 1) / 2) mod p is 1, 0 or p - 1; the result is selected without branching. */
    struct nm_mod md;
    struct crypto_bignum exp;
    uint16_t x[NM_I15_WORDS], one[NM_I15_WORDS];
    int res = -2;
    if (nm_mod_init(&md, p) < 0) {
        return -2;
    }
    bn_copy(&exp, p);
    bn_shr1(&exp); /* p odd: (p - 1) / 2 */
    nm_load(x, &md, a);
    if (nm_pow(x, &exp, &md) == 0) {
        const uint32_t zero = br_i15_iszero(x);
        uint32_t is_one;
        br_i15_zero(one, md.m[0]);
        one[1] = 1;
        br_i15_sub(x, one, 1);
        is_one = br_i15_iszero(x);
        res = (int)(MUX(is_one, 1, MUX(zero, 0, (uint32_t)-1)));
    }
    nm_crypto_zero(x, sizeof(x));
    nm_crypto_zero(one, sizeof(one));
    nm_crypto_zero(&exp, sizeof(exp));
    return res;
}

/* ---- P-256 -------------------------------------------------------------------------- */

static const u8 P256_P[NM_P256_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const u8 P256_N[NM_P256_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xBC, 0xE6, 0xFA, 0xAD, 0xA7, 0x17, 0x9E, 0x84, 0xF3, 0xB9,
    0xCA, 0xC2, 0xFC, 0x63, 0x25, 0x51};
static const u8 P256_B[NM_P256_LEN] = {0x5A, 0xC6, 0x35, 0xD8, 0xAA, 0x3A, 0x93, 0xE7, 0xB3, 0xEB,
    0xBD, 0x55, 0x76, 0x98, 0x86, 0xBC, 0x65, 0x1D, 0x06, 0xB0, 0xCC, 0x53, 0xB0, 0xF6, 0x3B, 0xCE,
    0x3C, 0x3E, 0x27, 0xD2, 0x60, 0x4B};

static int p256_load(struct crypto_ec* e) {
    struct crypto_bignum three;
    bn_set_uint(&three, 3);
    if (bn_from_be(&e->prime, P256_P, sizeof(P256_P)) < 0 ||
        bn_from_be(&e->order, P256_N, sizeof(P256_N)) < 0 ||
        bn_from_be(&e->b, P256_B, sizeof(P256_B)) < 0 || bn_sub(&e->prime, &three, &e->a) < 0) {
        return -1;
    }
    return 0;
}

struct crypto_ec* crypto_ec_init(int group) {
    struct crypto_ec* e;
    if (group != 19) {
        return NULL;
    }
    e = os_zalloc(sizeof(*e));
    if (e != NULL && p256_load(e) < 0) {
        os_free(e);
        e = NULL;
    }
    return e;
}

void crypto_ec_deinit(struct crypto_ec* e) {
    os_free(e);
}

size_t crypto_ec_prime_len(struct crypto_ec* e) {
    (void)e;
    return NM_P256_LEN;
}

size_t crypto_ec_prime_len_bits(struct crypto_ec* e) {
    return bn_bits(&e->prime);
}

size_t crypto_ec_order_len(struct crypto_ec* e) {
    (void)e;
    return NM_P256_LEN;
}

const struct crypto_bignum* crypto_ec_get_prime(struct crypto_ec* e) {
    return &e->prime;
}

const struct crypto_bignum* crypto_ec_get_order(struct crypto_ec* e) {
    return &e->order;
}

#if defined(NEWMOTA_HOSTAP_EC_A_BORROWED) && NEWMOTA_HOSTAP_EC_A_BORROWED
const struct crypto_bignum* crypto_ec_get_a(struct crypto_ec* e) {
    return &e->a;
}
#else
struct crypto_bignum* crypto_ec_get_a(struct crypto_ec* e) {
    struct crypto_bignum* a = crypto_bignum_init();
    if (a != NULL) {
        bn_copy(a, &e->a);
    }
    return a;
}
#endif

const struct crypto_bignum* crypto_ec_get_b(struct crypto_ec* e) {
    return &e->b;
}

struct crypto_ec_point* crypto_ec_point_init(struct crypto_ec* e) {
    struct crypto_ec_point* p;
    (void)e;
    p = os_zalloc(sizeof(*p));
    if (p != NULL) {
        p->infinity = 1;
    }
    return p;
}

void crypto_ec_point_deinit(struct crypto_ec_point* p, int clear) {
    (void)clear;
    if (p != NULL) {
        nm_crypto_zero(p, sizeof(*p));
        os_free(p);
    }
}

int crypto_ec_point_to_bin(struct crypto_ec* e, const struct crypto_ec_point* point, u8* x, u8* y) {
    (void)e;
    if (point->infinity) {
        return -1;
    }
    if (x != NULL) {
        memcpy(x, point->xy, NM_P256_LEN);
    }
    if (y != NULL) {
        memcpy(y, point->xy + NM_P256_LEN, NM_P256_LEN);
    }
    return 0;
}

struct crypto_ec_point* crypto_ec_point_from_bin(struct crypto_ec* e, const u8* val) {
    struct crypto_ec_point* p;
    if (e == NULL) {
        return NULL;
    }
    p = os_zalloc(sizeof(*p));
    if (p != NULL) {
        memcpy(p->xy, val, sizeof(p->xy));
    }
    return p;
}

struct crypto_bignum* crypto_ec_point_compute_y_sqr(
    struct crypto_ec* e, const struct crypto_bignum* x) {
    struct crypto_bignum* y2 = crypto_bignum_init();
    if (y2 != NULL && bn_y_sqr(e, x, y2) < 0) {
        crypto_bignum_deinit(y2, 1);
        y2 = NULL;
    }
    return y2;
}

int crypto_ec_point_is_at_infinity(struct crypto_ec* e, const struct crypto_ec_point* p) {
    (void)e;
    return p->infinity;
}

int crypto_ec_point_is_on_curve(struct crypto_ec* e, const struct crypto_ec_point* p) {
    struct crypto_bignum x, y, y2, rhs;
    int ok = 0;
    if (p->infinity) {
        return 0;
    }
    bn_from_be(&x, p->xy, NM_P256_LEN);
    bn_from_be(&y, p->xy + NM_P256_LEN, NM_P256_LEN);
    if (bn_cmp(&x, &e->prime) >= 0 || bn_cmp(&y, &e->prime) >= 0) {
        return 0;
    }
    /* On the stack and on the constant-time arithmetic: the PWE is password-derived. */
    if (bn_y_sqr(e, &x, &rhs) == 0 && crypto_bignum_mulmod(&y, &y, &e->prime, &y2) == 0) {
        ok = bn_cmp(&y2, &rhs) == 0;
    }
    return ok;
}

int crypto_ec_point_cmp(
    const struct crypto_ec* e, const struct crypto_ec_point* a, const struct crypto_ec_point* b) {
    (void)e;
    if (a->infinity || b->infinity) {
        return !(a->infinity && b->infinity);
    }
    return memcmp(a->xy, b->xy, sizeof(a->xy)) != 0;
}

int crypto_ec_point_invert(struct crypto_ec* e, struct crypto_ec_point* p) {
    struct crypto_bignum y;
    if (p->infinity) {
        return 0;
    }
    bn_from_be(&y, p->xy + NM_P256_LEN, NM_P256_LEN);
    if (bn_cmp(&y, &e->prime) >= 0) {
        return -1;
    }
    if (y.n != 0 && (bn_sub(&e->prime, &y, &y) < 0 || crypto_bignum_to_bin(&y, p->xy + NM_P256_LEN,
                                                          NM_P256_LEN, NM_P256_LEN) < 0)) {
        return -1;
    }
    return 0;
}

int crypto_ec_point_mul(struct crypto_ec* e, const struct crypto_ec_point* p,
    const struct crypto_bignum* b, struct crypto_ec_point* res) {
    u8 point[1 + 2 * NM_P256_LEN];
    u8 k[NM_P256_LEN];
    int ret = -1;
    /* As mbedtls_ecp_mul(): 1 <= k < n and a valid point. A valid point has order n, so the
       result is never the point at infinity. */
    if (b->n == 0 || bn_cmp(b, &e->order) >= 0 || !crypto_ec_point_is_on_curve(e, p)) {
        return -1;
    }
    point[0] = 0x04;
    memcpy(point + 1, p->xy, sizeof(p->xy));
    crypto_bignum_to_bin(b, k, sizeof(k), sizeof(k));
    if (br_ec_p256_m15.mul(point, sizeof(point), k, sizeof(k), BR_EC_secp256r1)) {
        res->infinity = 0;
        memcpy(res->xy, point + 1, sizeof(res->xy));
        ret = 0;
    }
    nm_crypto_zero(k, sizeof(k));
    nm_crypto_zero(point, sizeof(point));
    return ret;
}

int crypto_ec_point_add(struct crypto_ec* e, const struct crypto_ec_point* a,
    const struct crypto_ec_point* b, struct crypto_ec_point* c) {
    static const unsigned char one = 1;
    u8 pa[1 + 2 * NM_P256_LEN];
    u8 pb[1 + 2 * NM_P256_LEN];
    int ret = -1;
    /* As mbedtls_ecp_muladd() with both scalars 1: two valid points; the sum may be the
       point at infinity. BearSSL's muladd doubles equal points itself but reports a failure
       for a + (-a), so that case is decided here. */
    if (!crypto_ec_point_is_on_curve(e, a) || !crypto_ec_point_is_on_curve(e, b)) {
        return -1;
    }
    if (memcmp(a->xy, b->xy, NM_P256_LEN) == 0 &&
        memcmp(a->xy + NM_P256_LEN, b->xy + NM_P256_LEN, NM_P256_LEN) != 0) {
        c->infinity = 1;
        memset(c->xy, 0, sizeof(c->xy));
        return 0;
    }
    pa[0] = pb[0] = 0x04;
    memcpy(pa + 1, a->xy, sizeof(a->xy));
    memcpy(pb + 1, b->xy, sizeof(b->xy));
    if (br_ec_p256_m15.muladd(pa, pb, sizeof(pa), &one, 1, &one, 1, BR_EC_secp256r1)) {
        c->infinity = 0;
        memcpy(c->xy, pa + 1, sizeof(c->xy));
        ret = 0;
    }
    nm_crypto_zero(pa, sizeof(pa));
    nm_crypto_zero(pb, sizeof(pb));
    return ret;
}

/* ---- EC keys and ECDH (SAE-PK and OWE STA, P-256 only) ------------------------------ */

struct crypto_ec_key {
    u8 q[1 + 2 * NM_P256_LEN]; /* uncompressed public point, checked on the curve */
};

struct crypto_ecdh {
    u8 d[NM_P256_LEN];         /* private scalar, 1 <= d < n */
    u8 q[1 + 2 * NM_P256_LEN]; /* d * G, uncompressed */
};

/* SEQUENCE { OID id-ecPublicKey, OID prime256v1 }, the AlgorithmIdentifier of a P-256 key. */
static const u8 P256_SPKI_ALG[] = {0x30, 0x13, 0x06, 0x07, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01,
    0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};

/** 1 if xy (big-endian x || y) is a point on P-256 with coordinates below p. */
static int p256_valid_xy(const u8* xy) {
    struct crypto_ec e;
    struct crypto_ec_point pt;
    if (p256_load(&e) < 0) {
        return 0;
    }
    pt.infinity = 0;
    memcpy(pt.xy, xy, sizeof(pt.xy));
    return crypto_ec_point_is_on_curve(&e, &pt);
}

/** xy = (x, y) with y of the given parity: y = (x^3 - 3x + b)^((p + 1) / 4) mod p, as
    p = 3 mod 4. Fails if x >= p or no curve point has this x. */
static int p256_point_from_x(const u8* x, int y_odd, u8* xy) {
    struct crypto_ec e;
    struct crypto_bignum bx, y2, y, exp, one;
    struct nm_mod md;
    uint16_t t[NM_I15_WORDS];
    u8 out[2 * NM_P256_LEN];
    int ret = -1;
    if (p256_load(&e) < 0 || nm_mod_init(&md, &e.prime) < 0) {
        return -1;
    }
    bn_from_be(&bx, x, NM_P256_LEN);
    bn_set_uint(&one, 1);
    if (bn_cmp(&bx, &e.prime) < 0 && bn_y_sqr(&e, &bx, &y2) == 0 &&
        bn_add(&e.prime, &one, &exp) == 0) {
        bn_shr1(&exp);
        bn_shr1(&exp);
        nm_load(t, &md, &y2);
        if (nm_pow(t, &exp, &md) == 0 && nm_store(&y, &md, t) == 0 &&
            (crypto_bignum_is_odd(&y) == (y_odd != 0) || bn_sub(&e.prime, &y, &y) == 0)) {
            memcpy(out, x, NM_P256_LEN);
            crypto_bignum_to_bin(&y, out + NM_P256_LEN, NM_P256_LEN, NM_P256_LEN);
            /* A non-residue gives a y whose square is not y2; the curve check rejects it. */
            if (p256_valid_xy(out)) {
                memcpy(xy, out, sizeof(out));
                ret = 0;
            }
        }
    }
    nm_crypto_zero(t, sizeof(t));
    return ret;
}

struct crypto_ec_key* crypto_ec_key_parse_pub(const u8* der, size_t der_len) {
    /* SEQUENCE { AlgorithmIdentifier, BIT STRING { 0 unused bits, point } }: 91 bytes with an
       uncompressed point, 59 with a compressed one. */
    const size_t alg = sizeof(P256_SPKI_ALG);
    u8 xy[2 * NM_P256_LEN];
    struct crypto_ec_key* key;
    size_t point_len;
    if (der == NULL || (der_len != 2 + alg + 3 + 1 + 2 * NM_P256_LEN &&
                           der_len != 2 + alg + 3 + 1 + NM_P256_LEN)) {
        return NULL;
    }
    point_len = der_len - (2 + alg + 3);
    if (der[0] != 0x30 || der[1] != der_len - 2 || memcmp(der + 2, P256_SPKI_ALG, alg) != 0 ||
        der[2 + alg] != 0x03 || der[3 + alg] != point_len + 1 || der[4 + alg] != 0x00) {
        return NULL;
    }
    der += 5 + alg;
    if (point_len == 1 + 2 * NM_P256_LEN) {
        if (der[0] != 0x04 || !p256_valid_xy(der + 1)) {
            return NULL;
        }
        memcpy(xy, der + 1, sizeof(xy));
    } else if ((der[0] != 0x02 && der[0] != 0x03) ||
               p256_point_from_x(der + 1, der[0] == 0x03, xy) < 0) {
        return NULL;
    }
    key = os_zalloc(sizeof(*key));
    if (key != NULL) {
        key->q[0] = 0x04;
        memcpy(key->q + 1, xy, sizeof(xy));
    }
    return key;
}

int crypto_ec_key_group(struct crypto_ec_key* key) {
    return key != NULL ? 19 : -1;
}

void crypto_ec_key_deinit(struct crypto_ec_key* key) {
    os_free(key);
}

int crypto_ec_key_verify_signature(
    struct crypto_ec_key* key, const u8* data, size_t len, const u8* sig, size_t sig_len) {
    br_ec_public_key pk;
    if (key == NULL || data == NULL || sig == NULL) {
        return -1;
    }
    pk.curve = BR_EC_secp256r1;
    pk.q = key->q;
    pk.qlen = sizeof(key->q);
    return br_ecdsa_i15_vrfy_asn1(&br_ec_p256_m15, data, len, &pk, sig, sig_len) ? 1 : 0;
}

void crypto_ecdh_deinit(struct crypto_ecdh* ecdh) {
    if (ecdh != NULL) {
        nm_crypto_zero(ecdh, sizeof(*ecdh));
        os_free(ecdh);
    }
}

struct crypto_ecdh* crypto_ecdh_init(int group) {
    struct crypto_ecdh* ecdh;
    struct crypto_bignum n, d;
    if (group != 19) {
        return NULL;
    }
    ecdh = os_zalloc(sizeof(*ecdh));
    if (ecdh == NULL) {
        return NULL;
    }
    bn_from_be(&n, P256_N, sizeof(P256_N));
    /* d in [1, n): rejection sampling over [0, n), retried if it gives zero. */
    for (int attempt = 0; attempt < 8; attempt++) {
        if (crypto_bignum_rand(&d, &n) < 0) {
            break;
        }
        if (d.n != 0) {
            crypto_bignum_to_bin(&d, ecdh->d, sizeof(ecdh->d), sizeof(ecdh->d));
            nm_crypto_zero(&d, sizeof(d));
            /* The hardware provider's mulgen zeroes the result on failure. */
            if (br_ec_p256_m15.mulgen(ecdh->q, ecdh->d, sizeof(ecdh->d), BR_EC_secp256r1) ==
                    sizeof(ecdh->q) &&
                ecdh->q[0] == 0x04) {
                return ecdh;
            }
            break;
        }
    }
    nm_crypto_zero(&d, sizeof(d));
    crypto_ecdh_deinit(ecdh);
    return NULL;
}

struct wpabuf* crypto_ecdh_get_pubkey(struct crypto_ecdh* ecdh, int y) {
    if (ecdh == NULL) {
        return NULL;
    }
    return wpabuf_alloc_copy(ecdh->q + 1, y ? 2 * NM_P256_LEN : NM_P256_LEN);
}

struct wpabuf* crypto_ecdh_set_peerkey(
    struct crypto_ecdh* ecdh, int inc_y, const u8* key, size_t len) {
    u8 point[1 + 2 * NM_P256_LEN];
    struct wpabuf* secret = NULL;
    if (ecdh == NULL || key == NULL) {
        return NULL;
    }
    /* The peer key is x || y with inc_y, else x alone (OWE); y is then recovered with even
       parity, which leaves the x coordinate of the shared point unchanged. */
    point[0] = 0x04;
    if (inc_y) {
        if (len != 2 * NM_P256_LEN || !p256_valid_xy(key)) {
            return NULL;
        }
        memcpy(point + 1, key, 2 * NM_P256_LEN);
    } else if (len != NM_P256_LEN || p256_point_from_x(key, 0, point + 1) < 0) {
        return NULL;
    }
    if (br_ec_p256_m15.mul(point, sizeof(point), ecdh->d, sizeof(ecdh->d), BR_EC_secp256r1)) {
        secret = wpabuf_alloc_copy(point + 1, NM_P256_LEN);
    }
    nm_crypto_zero(point, sizeof(point));
    return secret;
}
