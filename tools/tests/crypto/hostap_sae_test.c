/*
  hostap_sae_test.c - hostap WPA3-SAE on the BearSSL crypto_bignum/crypto_ec backend

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file
 * @brief Runs the pinned ESP-IDF supplicant's unmodified sae.c and dragonfly.c on the host
 * against src/platform/crypto/hostap_sae_crypto.c and hostap_symmetric_bearssl.c: known
 * answers for MD5, SHA-1, SHA-256, HMAC-SHA1, PBKDF2-SHA1, AES, key wrap and CMAC; IEEE Std
 * 802.11-2020 Annex J.10 vectors (as in hostap's common_module_tests.c and ESP-IDF's test_sae.c),
 * random handshakes with hunting-and-pecking and hash-to-element, rejection of a wrong password and
 * of tampered commits, and edge cases of the bignum/EC API. Exit status is the failure count
 * (capped).
 */
#include "utils/includes.h"
#include "utils/common.h"
#include "crypto/crypto.h"
#include "common/sae.h"
#include "utils/wpabuf.h"
#include "crypto/aes_wrap.h"
#include "crypto/sha1.h"
#include "crypto/sha256.h"
#include "crypto/aes_siv.h"
#include <stdio.h>
#include <stdlib.h>

const wifi_osi_funcs_t* wifi_funcs;
static int failures;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            failures++;                                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                           \
        }                                                                                          \
    } while (0)

/* Bytes os_get_random returns next instead of random ones, for a known ECDH private key. */
static const u8* forced_random;
static size_t forced_random_len;

/* /dev/urandom rather than arc4random, which glibc has only from 2.36. */
int os_get_random(unsigned char* buf, size_t len) {
    static FILE* f;
    if (forced_random != NULL) {
        int ok = len == forced_random_len;
        if (ok)
            os_memcpy(buf, forced_random, len);
        forced_random = NULL;
        return ok ? 0 : -1;
    }
    if (f == NULL)
        f = fopen("/dev/urandom", "rb");
    return f != NULL && fread(buf, 1, len, f) == len ? 0 : -1;
}

unsigned long os_random(void) {
    unsigned long r = 0;
    os_get_random((unsigned char*)&r, sizeof(r));
    return r;
}

int os_get_time(struct os_time* t) {
    t->sec = 0;
    t->usec = 0;
    return 0;
}

u32 wpa_akm_to_suite(int akm) {
    (void)akm;
    return 0;
}

int crypto_mod_exp(
    const u8* b, size_t bl, const u8* e, size_t el, const u8* m, size_t ml, u8* r, size_t* rl) {
    (void)b;
    (void)bl;
    (void)e;
    (void)el;
    (void)m;
    (void)ml;
    (void)r;
    (void)rl;
    return -1;
}

static void vectors(void) {
    struct sae_data sae;
    const u8 addr1[ETH_ALEN] = {0x4d, 0x3f, 0x2f, 0xff, 0xe3, 0x87};
    const u8 addr2[ETH_ALEN] = {0xa5, 0xd8, 0xaa, 0x95, 0x8e, 0x3c};
    const char* ssid = "byteme";
    const char* pw = "mekmitasdigoat";
    const char* pwid = "psk4internet";
    const u8 local_rand[] = {0x99, 0x24, 0x65, 0xfd, 0x3d, 0xaa, 0x3c, 0x60, 0xaa, 0x65, 0x65, 0xb7,
        0xf6, 0x2a, 0x2a, 0x7f, 0x2e, 0x12, 0xdd, 0x12, 0xf1, 0x98, 0xfa, 0xf4, 0xfb, 0xed, 0x89,
        0xd7, 0xff, 0x1a, 0xce, 0x94};
    const u8 local_mask[] = {0x95, 0x07, 0xa9, 0x0f, 0x77, 0x7a, 0x04, 0x4d, 0x6a, 0x08, 0x30, 0xb9,
        0x1e, 0xa3, 0xd5, 0xdd, 0x70, 0xbe, 0xce, 0x44, 0xe1, 0xac, 0xff, 0xb8, 0x69, 0x83, 0xb5,
        0xe1, 0xbf, 0x9f, 0xb3, 0x22};
    const u8 local_commit[] = {0x13, 0x00, 0x2e, 0x2c, 0x0f, 0x0d, 0xb5, 0x24, 0x40, 0xad, 0x14,
        0x6d, 0x96, 0x71, 0x14, 0xce, 0x00, 0x5c, 0xe1, 0xea, 0xb0, 0xaa, 0x2c, 0x2e, 0x5c, 0x28,
        0x71, 0xb7, 0x74, 0xf6, 0xc2, 0x57, 0x5c, 0x65, 0xd5, 0xad, 0x9e, 0x00, 0x82, 0x97, 0x07,
        0xaa, 0x36, 0xba, 0x8b, 0x85, 0x97, 0x38, 0xfc, 0x96, 0x1d, 0x08, 0x24, 0x35, 0x05, 0xf4,
        0x7c, 0x03, 0x53, 0x76, 0xd7, 0xac, 0x4b, 0xc8, 0xd7, 0xb9, 0x50, 0x83, 0xbf, 0x43, 0x82,
        0x7d, 0x0f, 0xc3, 0x1e, 0xd7, 0x78, 0xdd, 0x36, 0x71, 0xfd, 0x21, 0xa4, 0x6d, 0x10, 0x91,
        0xd6, 0x4b, 0x6f, 0x9a, 0x1e, 0x12, 0x72, 0x62, 0x13, 0x25, 0xdb, 0xe1};
    const u8 peer_commit[] = {0x13, 0x00, 0x59, 0x1b, 0x96, 0xf3, 0x39, 0x7f, 0xb9, 0x45, 0x10,
        0x08, 0x48, 0xe7, 0xb5, 0x50, 0x54, 0x3b, 0x67, 0x20, 0xd8, 0x83, 0x37, 0xee, 0x93, 0xfc,
        0x49, 0xfd, 0x6d, 0xf7, 0xe0, 0x8b, 0x52, 0x23, 0xe7, 0x1b, 0x9b, 0xb0, 0x48, 0xd3, 0x87,
        0x3f, 0x20, 0x55, 0x69, 0x53, 0xa9, 0x6c, 0x91, 0x53, 0x6f, 0xd8, 0xee, 0x6c, 0xa9, 0xb4,
        0xa6, 0x8a, 0x14, 0x8b, 0x05, 0x6a, 0x90, 0x9b, 0xe0, 0x3e, 0x83, 0xae, 0x20, 0x8f, 0x60,
        0xf8, 0xef, 0x55, 0x37, 0x85, 0x80, 0x74, 0xdb, 0x06, 0x68, 0x70, 0x32, 0x39, 0x98, 0x62,
        0x99, 0x9b, 0x51, 0x1e, 0x0a, 0x15, 0x52, 0xa5, 0xfe, 0xa3, 0x17, 0xc2};
    const u8 kck[] = {0x1e, 0x73, 0x3f, 0x6d, 0x9b, 0xd5, 0x32, 0x56, 0x28, 0x73, 0x04, 0x33, 0x88,
        0x31, 0xb0, 0x9a, 0x39, 0x40, 0x6d, 0x12, 0x10, 0x17, 0x07, 0x3a, 0x5c, 0x30, 0xdb, 0x36,
        0xf3, 0x6c, 0xb8, 0x1a};
    const u8 pmk[] = {0x4e, 0x4d, 0xfa, 0xb1, 0xa2, 0xdd, 0x8a, 0xc1, 0xa9, 0x17, 0x90, 0xf9, 0x53,
        0xfa, 0xaa, 0x45, 0x2a, 0xe5, 0xc6, 0x87, 0x3a, 0xb7, 0x5b, 0x63, 0x60, 0x5b, 0xa6, 0x63,
        0xf8, 0xa7, 0xfe, 0x59};
    const u8 pmkid[] = {0x87, 0x47, 0xa6, 0x00, 0xee, 0xa3, 0xf9, 0xf2, 0x24, 0x75, 0xdf, 0x58,
        0xca, 0x1e, 0x54, 0x98};

    const u8 pwe_19_x[32] = {0xc9, 0x30, 0x49, 0xb9, 0xe6, 0x40, 0x00, 0xf8, 0x48, 0x20, 0x16, 0x49,
        0xe9, 0x99, 0xf2, 0xb5, 0xc2, 0x2d, 0xea, 0x69, 0xb5, 0x63, 0x2c, 0x9d, 0xf4, 0xd6, 0x33,
        0xb8, 0xaa, 0x1f, 0x6c, 0x1e};
    const u8 pwe_19_y[32] = {0x73, 0x63, 0x4e, 0x94, 0xb5, 0x3d, 0x82, 0xe7, 0x38, 0x3a, 0x8d, 0x25,
        0x81, 0x99, 0xd9, 0xdc, 0x1a, 0x5e, 0xe8, 0x26, 0x9d, 0x06, 0x03, 0x82, 0xcc, 0xbf, 0x33,
        0xe6, 0x14, 0xff, 0x59, 0xa0};
    struct wpabuf* buf = wpabuf_alloc(1000);
    struct crypto_bignum* mask;
    int pt_groups[] = {19, 0};
    struct sae_pt* pt;
    const u8 addr1b[ETH_ALEN] = {0x00, 0x09, 0x5b, 0x66, 0xec, 0x1e};
    const u8 addr2b[ETH_ALEN] = {0x00, 0x0b, 0x6b, 0xd9, 0x02, 0x46};

    os_memset(&sae, 0, sizeof(sae));
    CHECK(buf != NULL);
    CHECK(sae_set_group(&sae, 19) == 0);
    CHECK(sae_prepare_commit(addr1, addr2, (const u8*)pw, os_strlen(pw), &sae) == 0);
    crypto_bignum_deinit(sae.tmp->sae_rand, 1);
    sae.tmp->sae_rand = crypto_bignum_init_set(local_rand, sizeof(local_rand));
    mask = crypto_bignum_init_set(local_mask, sizeof(local_mask));
    CHECK(crypto_bignum_add(sae.tmp->sae_rand, mask, sae.tmp->own_commit_scalar) == 0);
    CHECK(crypto_bignum_mod(
              sae.tmp->own_commit_scalar, sae.tmp->order, sae.tmp->own_commit_scalar) == 0);
    CHECK(crypto_ec_point_mul(
              sae.tmp->ec, sae.tmp->pwe_ecc, mask, sae.tmp->own_commit_element_ecc) == 0);
    CHECK(crypto_ec_point_invert(sae.tmp->ec, sae.tmp->own_commit_element_ecc) == 0);
    CHECK(sae_write_commit(&sae, buf, NULL, NULL) == 0);
    CHECK(wpabuf_len(buf) == sizeof(local_commit));
    CHECK(os_memcmp(wpabuf_head(buf), local_commit, sizeof(local_commit)) == 0);
    CHECK(sae_parse_commit(&sae, peer_commit, sizeof(peer_commit), NULL, NULL, NULL, 0) == 0);
    CHECK(sae_process_commit(&sae) == 0);
    CHECK(os_memcmp(kck, sae.tmp->kck, SAE_KCK_LEN) == 0);
    CHECK(os_memcmp(pmk, sae.pmk, SAE_PMK_LEN) == 0);
    CHECK(os_memcmp(pmkid, sae.pmkid, SAE_PMKID_LEN) == 0);

    pt = sae_derive_pt(
        pt_groups, (const u8*)ssid, os_strlen(ssid), (const u8*)pw, os_strlen(pw), pwid);
    CHECK(pt != NULL);
    if (pt) {
        u8 bin[64];
        struct crypto_ec_point* pwe = sae_derive_pwe_from_pt_ecc(pt, addr1b, addr2b);
        CHECK(pwe != NULL);
        if (pwe) {
            CHECK(crypto_ec_point_to_bin(pt->ec, pwe, bin, bin + 32) == 0);
            CHECK(os_memcmp(pwe_19_x, bin, 32) == 0);
            CHECK(os_memcmp(pwe_19_y, bin + 32, 32) == 0);
            crypto_ec_point_deinit(pwe, 1);
        }
        sae_deinit_pt(pt);
    }
    sae_clear_data(&sae);
    wpabuf_free(buf);
    crypto_bignum_deinit(mask, 1);
}

/* One SAE exchange between two peers; returns 0 when both confirms verify. */
static int handshake(const char* pw1, const char* pw2, int h2e, u8* pmk1, u8* pmk2, int tamper) {
    struct sae_data s1, s2;
    u8 a1[ETH_ALEN], a2[ETH_ALEN];
    int groups[] = {19, 0};
    struct sae_pt *pt1 = NULL, *pt2 = NULL;
    struct wpabuf *c1 = wpabuf_alloc(SAE_COMMIT_MAX_LEN), *c2 = wpabuf_alloc(SAE_COMMIT_MAX_LEN);
    struct wpabuf *f1 = wpabuf_alloc(SAE_COMMIT_MAX_LEN), *f2 = wpabuf_alloc(SAE_COMMIT_MAX_LEN);
    int ret = -1;
    os_get_random(a1, sizeof(a1));
    os_get_random(a2, sizeof(a2));
    os_memset(&s1, 0, sizeof(s1));
    os_memset(&s2, 0, sizeof(s2));
    if (sae_set_group(&s1, 19) || sae_set_group(&s2, 19))
        goto out;
    if (h2e) {
        pt1 = sae_derive_pt(groups, (const u8*)"ssid", 4, (const u8*)pw1, strlen(pw1), NULL);
        pt2 = sae_derive_pt(groups, (const u8*)"ssid", 4, (const u8*)pw2, strlen(pw2), NULL);
        if (!pt1 || !pt2 || sae_prepare_commit_pt(&s1, pt1, a1, a2, NULL, NULL) ||
            sae_prepare_commit_pt(&s2, pt2, a2, a1, NULL, NULL))
            goto out;
    } else if (sae_prepare_commit(a1, a2, (const u8*)pw1, strlen(pw1), &s1) ||
               sae_prepare_commit(a2, a1, (const u8*)pw2, strlen(pw2), &s2)) {
        goto out;
    }
    if (sae_write_commit(&s1, c1, NULL, NULL) || sae_write_commit(&s2, c2, NULL, NULL))
        goto out;
    if (tamper == 1) /* element y += 1: off the curve */
        wpabuf_mhead_u8(c2)[2 + 32 + 63] ^= 1;
    if (tamper == 2) /* scalar = 0 */
        os_memset(wpabuf_mhead_u8(c2) + 2, 0, 32);
    if (tamper == 3) /* scalar = r (order) */
        os_memcpy(wpabuf_mhead_u8(c2) + 2,
            "\xff\xff\xff\xff\x00\x00\x00\x00\xff\xff\xff\xff\xff\xff\xff\xff\xbc\xe6\xfa\xad\xa7"
            "\x17\x9e\x84\xf3\xb9\xca\xc2\xfc\x63\x25\x51",
            32);
    s1.state = s2.state = SAE_COMMITTED;
    if (sae_parse_commit(&s1, wpabuf_head(c2), wpabuf_len(c2), NULL, NULL, groups, h2e) != 0 ||
        sae_parse_commit(&s2, wpabuf_head(c1), wpabuf_len(c1), NULL, NULL, groups, h2e) != 0)
        goto out;
    if (sae_process_commit(&s1) || sae_process_commit(&s2))
        goto out;
    if (sae_write_confirm(&s1, f1) || sae_write_confirm(&s2, f2))
        goto out;
    if (sae_check_confirm(&s1, wpabuf_head(f2), wpabuf_len(f2)) ||
        sae_check_confirm(&s2, wpabuf_head(f1), wpabuf_len(f1)))
        goto out;
    os_memcpy(pmk1, s1.pmk, SAE_PMK_LEN);
    os_memcpy(pmk2, s2.pmk, SAE_PMK_LEN);
    ret = 0;
out:
    sae_clear_data(&s1);
    sae_clear_data(&s2);
    sae_deinit_pt(pt1);
    sae_deinit_pt(pt2);
    wpabuf_free(c1);
    wpabuf_free(c2);
    wpabuf_free(f1);
    wpabuf_free(f2);
    return ret;
}

static struct crypto_bignum* bn_hex(const char* hex) {
    u8 b[128];
    size_t n = strlen(hex) / 2;
    for (size_t i = 0; i < n; i++)
        sscanf(hex + 2 * i, "%2hhx", &b[i]);
    return crypto_bignum_init_set(b, n);
}

static void api_edges(void) {
    struct crypto_ec* e = crypto_ec_init(19);
    struct crypto_bignum *one = crypto_bignum_init_uint(1), *zero = crypto_bignum_init_uint(0);
    struct crypto_bignum* t = crypto_bignum_init();
    struct crypto_ec_point *g, *p = crypto_ec_point_init(e), *q = crypto_ec_point_init(e);
    const struct crypto_bignum *n = crypto_ec_get_order(e), *prime = crypto_ec_get_prime(e);
    u8 gxy[64];
    CHECK(crypto_ec_init(20) == NULL);
    /* G of P-256 */
    struct crypto_bignum* gx =
        bn_hex("6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296");
    struct crypto_bignum* gy =
        bn_hex("4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5");
    crypto_bignum_to_bin(gx, gxy, 32, 32);
    crypto_bignum_to_bin(gy, gxy + 32, 32, 32);
    g = crypto_ec_point_from_bin(e, gxy);
    CHECK(crypto_ec_point_is_on_curve(e, g));
    CHECK(crypto_ec_point_is_at_infinity(e, p));
    CHECK(crypto_ec_point_mul(e, g, zero, p) < 0); /* k = 0 rejected */
    CHECK(crypto_ec_point_mul(e, g, n, p) < 0);    /* k = n rejected */
    CHECK(crypto_ec_point_mul(e, g, one, p) == 0 && crypto_ec_point_cmp(e, p, g) == 0);
    /* 2G two ways */
    struct crypto_bignum* two = crypto_bignum_init_uint(2);
    CHECK(crypto_ec_point_mul(e, g, two, p) == 0);
    CHECK(crypto_ec_point_add(e, g, g, q) == 0 && crypto_ec_point_cmp(e, p, q) == 0);
    /* (n-1)G = -G; G + (-G) = infinity */
    CHECK(crypto_bignum_sub(n, one, t) == 0);
    CHECK(crypto_ec_point_mul(e, g, t, p) == 0);
    struct crypto_ec_point* ng = crypto_ec_point_from_bin(e, gxy);
    CHECK(crypto_ec_point_invert(e, ng) == 0 && crypto_ec_point_cmp(e, p, ng) == 0);
    CHECK(crypto_ec_point_add(e, g, ng, q) == 0 && crypto_ec_point_is_at_infinity(e, q));
    CHECK(crypto_ec_point_mul(e, q, two, p) < 0); /* infinity is not a valid input */
    CHECK(crypto_ec_point_to_bin(e, q, gxy, NULL) < 0);
    /* 3G = 2G + G == 3 * G */
    struct crypto_bignum* three = crypto_bignum_init_uint(3);
    struct crypto_ec_point *g2 = crypto_ec_point_init(e), *g3a = crypto_ec_point_init(e),
                           *g3b = crypto_ec_point_init(e);
    CHECK(crypto_ec_point_mul(e, g, two, g2) == 0 && crypto_ec_point_add(e, g2, g, g3a) == 0);
    CHECK(crypto_ec_point_mul(e, g, three, g3b) == 0 && crypto_ec_point_cmp(e, g3a, g3b) == 0);
    /* bignum: inverse, legendre, exptmod, div, rshift, to_bin padding, sub underflow */
    struct crypto_bignum* a =
        bn_hex("1234567890ABCDEF1234567890ABCDEF1234567890ABCDEF1234567890ABCDEF");
    struct crypto_bignum *inv = crypto_bignum_init(), *chk = crypto_bignum_init();
    CHECK(crypto_bignum_inverse(a, prime, inv) == 0 &&
          crypto_bignum_mulmod(a, inv, prime, chk) == 0 && crypto_bignum_is_one(chk));
    CHECK(crypto_bignum_inverse(a, n, inv) == 0 && crypto_bignum_mulmod(a, inv, n, chk) == 0 &&
          crypto_bignum_is_one(chk));
    CHECK(crypto_bignum_inverse(zero, prime, inv) < 0);
    CHECK(crypto_bignum_legendre(zero, prime) == 0);
    CHECK(crypto_bignum_legendre(one, prime) == 1);
    /* p = 3 mod 4: -1 is a non-residue */
    CHECK(crypto_bignum_sub(prime, one, t) == 0 && crypto_bignum_legendre(t, prime) == -1);
    /* Fermat: a^(p-1) = 1 */
    CHECK(crypto_bignum_exptmod(a, t, prime, chk) == 0 && crypto_bignum_is_one(chk));
    CHECK(crypto_bignum_sub(zero, one, t) < 0);
    CHECK(crypto_bignum_div(a, two, t) == 0 && crypto_bignum_rshift(a, 1, chk) == 0 &&
          crypto_bignum_cmp(t, chk) == 0);
    u8 pad[40];
    CHECK(crypto_bignum_to_bin(one, pad, sizeof(pad), 32) == 32 && pad[31] == 1 && pad[0] == 0);
    CHECK(crypto_bignum_to_bin(zero, pad, sizeof(pad), 0) == 0);
    CHECK(crypto_bignum_to_bin(a, pad, 16, 0) < 0);
    /* rand stays below the modulus */
    for (int i = 0; i < 2000; i++)
        CHECK(crypto_bignum_rand(t, three) == 0 && crypto_bignum_cmp(t, three) < 0);
    /* capacity: 1536-bit FFC prime is refused */
    u8 big[192];
    memset(big, 0xff, sizeof(big));
    CHECK(crypto_bignum_init_set(big, sizeof(big)) == NULL);
    crypto_bignum_deinit(a, 1);
    crypto_bignum_deinit(inv, 1);
    crypto_bignum_deinit(chk, 1);
    crypto_bignum_deinit(gx, 0);
    crypto_bignum_deinit(gy, 0);
    crypto_bignum_deinit(two, 0);
    crypto_bignum_deinit(three, 0);
    crypto_bignum_deinit(one, 0);
    crypto_bignum_deinit(zero, 0);
    crypto_bignum_deinit(t, 1);
    crypto_ec_point_deinit(g, 0);
    crypto_ec_point_deinit(p, 0);
    crypto_ec_point_deinit(q, 0);
    crypto_ec_point_deinit(ng, 0);
    crypto_ec_point_deinit(g2, 0);
    crypto_ec_point_deinit(g3a, 0);
    crypto_ec_point_deinit(g3b, 0);
    crypto_ec_deinit(e);
}

static void unhex(const char* hex, u8* out) {
    for (size_t i = 0; hex[2 * i]; i++)
        sscanf(hex + 2 * i, "%2hhx", &out[i]);
}

/* Known answers for the primitives that replace the supplicant's internal crypto. */
static void symmetric(void) {
    const u8* abc[] = {(const u8*)"abc"};
    const size_t abc_len[] = {3};
    u8 out[64], exp[64], key[32], blk[16], ct[24];
    unhex("900150983cd24fb0d6963f7d28e17f72", exp);
    CHECK(md5_vector(1, abc, abc_len, out) == 0 && os_memcmp(out, exp, 16) == 0);
    unhex("a9993e364706816aba3e25717850c26c9cd0d89d", exp);
    CHECK(sha1_vector(1, abc, abc_len, out) == 0 && os_memcmp(out, exp, 20) == 0);
    unhex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", exp);
    CHECK(sha256_vector(1, abc, abc_len, out) == 0 && os_memcmp(out, exp, 32) == 0);
    /* RFC 2202 HMAC-SHA1 test 1 */
    os_memset(key, 0x0b, 20);
    unhex("b617318655057264e28bc0b6fb378c8ef146be00", exp);
    CHECK(hmac_sha1(key, 20, (const u8*)"Hi There", 8, out) == 0 && os_memcmp(out, exp, 20) == 0);
    /* IEEE Std 802.11i-2004, H.4.3: WPA2 PSK = PBKDF2-HMAC-SHA1(passphrase, SSID, 4096, 32) */
    unhex("f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e", exp);
    CHECK(pbkdf2_sha1("password", (const u8*)"IEEE", 4, 4096, out, 32) == 0 &&
          os_memcmp(out, exp, 32) == 0);
    unhex("0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af", exp);
    CHECK(pbkdf2_sha1("ThisIsAPassword", (const u8*)"ThisIsASSID", 11, 4096, out, 32) == 0 &&
          os_memcmp(out, exp, 32) == 0);
    /* FIPS-197 C.1 and C.3 */
    for (int bits = 128; bits <= 256; bits += 128) {
        void *e, *d;
        unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key);
        unhex("00112233445566778899aabbccddeeff", blk);
        unhex(bits == 128 ? "69c4e0d86a7b0430d8cdb78070b4c55a" : "8ea2b7ca516745bfeafc49904b496089",
            exp);
        e = aes_encrypt_init(key, (size_t)bits / 8);
        d = aes_decrypt_init(key, (size_t)bits / 8);
        CHECK(e != NULL && d != NULL);
        if (e && d) {
            CHECK(aes_encrypt(e, blk, out) == 0 && os_memcmp(out, exp, 16) == 0);
            CHECK(aes_decrypt(d, out, out + 16) == 0 && os_memcmp(out + 16, blk, 16) == 0);
            CHECK(aes_encrypt(e, blk, blk) == 0 && os_memcmp(blk, exp, 16) == 0); /* in place */
        }
        aes_encrypt_deinit(e);
        aes_decrypt_deinit(d);
    }
    CHECK(aes_encrypt_init(key, 20) == NULL);
    /* RFC 3394 4.1: AES-128 key wrap of 128 bits */
    unhex("000102030405060708090a0b0c0d0e0f", key);
    unhex("00112233445566778899aabbccddeeff", blk);
    unhex("1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5", exp);
    CHECK(aes_wrap(key, 16, 2, blk, ct) == 0 && os_memcmp(ct, exp, 24) == 0);
    CHECK(aes_unwrap(key, 16, 2, ct, out) == 0 && os_memcmp(out, blk, 16) == 0);
    ct[0] ^= 1;
    CHECK(aes_unwrap(key, 16, 2, ct, out) != 0);
    /* RFC 4493 example 1: AES-CMAC of the empty message */
    unhex("2b7e151628aed2a6abf7158809cf4f3c", key);
    unhex("bb1d6929e95937287fa37d129b756746", exp);
    CHECK(omac1_aes_128(key, (const u8*)"", 0, out) == 0 && os_memcmp(out, exp, 16) == 0);
}

/* SAE-PK and OWE: AES-SIV, HKDF-SHA256, P-256 key parsing, ECDSA verify and ECDH. The P-256
   vectors were made with OpenSSL 4.1 (ecparam, ec -conv_form, dgst -sign, pkeyutl -derive). */
static const char A_PUB[] = "3059301306072a8648ce3d020106082a8648ce3d03010703420004"
                            "f409fbb909d103a84c6994863f24a4ee2082e68cf52f61d7e3b0d5b0d06dfbd0"
                            "3c4e42b521fae14a5310f5e30090f462b501ca2184723d68af23d37201baa2c8";
static const char A_PUBC[] = "3039301306072a8648ce3d020106082a8648ce3d03010703220002"
                             "f409fbb909d103a84c6994863f24a4ee2082e68cf52f61d7e3b0d5b0d06dfbd0";
static const char A_PRIV[] = "805ac70d5716db97a68256aa8e1f1a83de6565c24a140ce61691d31f9a2cec7b";
static const char B_X[] = "80f9cbc12be65d09037aa7e770b61311ea6bac8ebabd2a475dde61ac7a2ded2a";
static const char HASH[] = "bf551c2f80f68a9fc867a60f944402e4aa8a58c860c6ea0f4fdad2295f16f650";
static const char SIG[] = "304502200c2a1022c5c01ad8ee0b3a9cdfe47397d1911d4aafb474debc6c072130824195"
                          "022100a9729d1aab5a22aeaba8df9870e43e2c8a97f72c41c0893d1d9d03a660f53514";
/* A second OpenSSL key whose y is odd (compressed prefix 03), signing the same message. */
static const char O_PUB[] = "3059301306072a8648ce3d020106082a8648ce3d03010703420004"
                            "1bdaa1bddbfe46e449099a22e220ce5dfd6c300d0cbf0762638d42856db8bc02"
                            "5bb08b02efd4745821eea3817b4f59d6c28dda29c0a3d205355054852c761f45";
static const char O_PUBC[] = "3039301306072a8648ce3d020106082a8648ce3d03010703220003"
                             "1bdaa1bddbfe46e449099a22e220ce5dfd6c300d0cbf0762638d42856db8bc02";
static const char O_SIG[] =
    "304602210094ac83fe502c201a63165b5eeb87b567bb1d0ae8e60e93279a1d4e75493f1bf2"
    "022100efa0837edd640af6fe72b8f6cd2fb91d101604153710beacd8824a7f2f77f0d0";
static const char Z_AB[] = "f6bafd308dd471f7f7d9bdb698145c78ba27f33df98c498f70530e46b99b2c92";
static const char C_PUB_P384[] = "3076301006072a8648ce3d020106052b8104002203620004"
                                 "535a24c8e5416800b0b0758afe413b83a637abafe7988a2c1a4460a4ac02702de"
                                 "f64fd7c8de1af1f887481c4f9ff958d"
                                 "5e52295e1b280068a840cb1dd355e9996c59252b9a8187d288cd09215c985a1ba"
                                 "11801df88bf72a931bde103b318fa84";

static void sae_pk_owe(void) {
    u8 der[120], der2[120], hash[32], sig[80], x[32], d[32], z[32], key[64], in[32], out[64];
    const size_t der_len = strlen(A_PUB) / 2, derc_len = strlen(A_PUBC) / 2,
                 sig_len = strlen(SIG) / 2;
    struct crypto_ec_key* k;
    struct crypto_ecdh *e1, *e2;
    struct wpabuf *p1, *p2, *s1, *s2;
    struct crypto_ec* ec = crypto_ec_init(19);
    int rejected = 0;

    /* RFC 5297 A.1: AES-SIV with one associated-data element. */
    {
        const u8* ad[1];
        size_t ad_len[1] = {24};
        u8 ad1[24], pt[14];
        unhex("fffefdfcfbfaf9f8f7f6f5f4f3f2f1f0f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", key);
        unhex("101112131415161718191a1b1c1d1e1f2021222324252627", ad1);
        unhex("112233445566778899aabbccddee", pt);
        unhex("85632d07c6e8f37f950acd320a2ecc9340c02b9690c4dc04daef7f6afe5c", der2);
        ad[0] = ad1;
        CHECK(aes_siv_encrypt(key, 32, pt, 14, 1, ad, ad_len, out) == 0 &&
              os_memcmp(out, der2, 30) == 0);
        CHECK(aes_siv_decrypt(key, 32, out, 30, 1, ad, ad_len, der) == 0 &&
              os_memcmp(der, pt, 14) == 0);
        out[3] ^= 1;
        CHECK(aes_siv_decrypt(key, 32, out, 30, 1, ad, ad_len, der) != 0);
    }
    /* RFC 5869 A.1: HKDF-SHA256 extract (hmac_sha256) and expand (hmac_sha256_kdf, no label),
       as OWE derives its PMK. */
    {
        u8 ikm[22], salt[13], info[10], prk[32];
        os_memset(ikm, 0x0b, sizeof(ikm));
        unhex("000102030405060708090a0b0c", salt);
        unhex("f0f1f2f3f4f5f6f7f8f9", info);
        unhex("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5", der2);
        CHECK(hmac_sha256(salt, sizeof(salt), ikm, sizeof(ikm), prk) == 0 &&
              os_memcmp(prk, der2, 32) == 0);
        unhex(
            "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865",
            der2);
        CHECK(hmac_sha256_kdf(prk, 32, NULL, info, sizeof(info), out, 42) == 0 &&
              os_memcmp(out, der2, 42) == 0);
    }

    unhex(A_PUB, der);
    unhex(HASH, hash);
    unhex(SIG, sig);
    /* Uncompressed and compressed SubjectPublicKeyInfo give the same key. */
    for (int c = 0; c < 2; c++) {
        size_t len = c ? derc_len : der_len;
        unhex(c ? A_PUBC : A_PUB, der2);
        k = crypto_ec_key_parse_pub(der2, len);
        CHECK(k != NULL);
        if (!k)
            continue;
        CHECK(crypto_ec_key_group(k) == 19);
        CHECK(crypto_ec_key_verify_signature(k, hash, 32, sig, sig_len) == 1);
        hash[5] ^= 1;
        CHECK(crypto_ec_key_verify_signature(k, hash, 32, sig, sig_len) == 0);
        hash[5] ^= 1;
        sig[sig_len - 1] ^= 1;
        CHECK(crypto_ec_key_verify_signature(k, hash, 32, sig, sig_len) == 0);
        sig[sig_len - 1] ^= 1;
        CHECK(crypto_ec_key_verify_signature(k, hash, 32, sig, sig_len - 1) == 0);
        crypto_ec_key_deinit(k);
    }
    /* Odd y: both encodings verify; prefix 02 gives the other y, which does not. */
    {
        u8 osig[80];
        const size_t osig_len = strlen(O_SIG) / 2;
        unhex(O_SIG, osig);
        for (int c = 0; c < 3; c++) {
            unhex(c ? O_PUBC : O_PUB, der2);
            if (c == 2)
                der2[26] = 0x02;
            k = crypto_ec_key_parse_pub(der2, c ? strlen(O_PUBC) / 2 : strlen(O_PUB) / 2);
            CHECK(k != NULL &&
                  crypto_ec_key_verify_signature(k, hash, 32, osig, osig_len) == (c < 2));
            crypto_ec_key_deinit(k);
        }
    }
    /* The other y (prefix 03) parses but is a different key. */
    unhex(A_PUBC, der2);
    der2[26] = 0x03;
    k = crypto_ec_key_parse_pub(der2, derc_len);
    CHECK(k != NULL && crypto_ec_key_verify_signature(k, hash, 32, sig, sig_len) == 0);
    crypto_ec_key_deinit(k);
    /* Rejected: P-384, truncation, extra byte, wrong lengths, off-curve point, x >= p. */
    unhex(C_PUB_P384, der2);
    CHECK(crypto_ec_key_parse_pub(der2, strlen(C_PUB_P384) / 2) == NULL);
    CHECK(crypto_ec_key_parse_pub(der, der_len - 1) == NULL);
    os_memcpy(der2, der, der_len);
    der2[der_len] = 0;
    CHECK(crypto_ec_key_parse_pub(der2, der_len + 1) == NULL);
    der2[1] ^= 1;
    CHECK(crypto_ec_key_parse_pub(der2, der_len) == NULL);
    os_memcpy(der2, der, der_len);
    der2[25] = 1; /* unused bits */
    CHECK(crypto_ec_key_parse_pub(der2, der_len) == NULL);
    os_memcpy(der2, der, der_len);
    der2[26] = 0x02; /* compressed tag on an uncompressed length */
    CHECK(crypto_ec_key_parse_pub(der2, der_len) == NULL);
    os_memcpy(der2, der, der_len);
    der2[der_len - 1] ^= 1;
    CHECK(crypto_ec_key_parse_pub(der2, der_len) == NULL);
    unhex(A_PUBC, der2);
    os_memset(der2 + 27, 0xff, 32);
    CHECK(crypto_ec_key_parse_pub(der2, derc_len) == NULL);
    CHECK(crypto_ec_key_verify_signature(NULL, hash, 32, sig, sig_len) == -1);

    /* OWE: known private key (through os_get_random), peer x only, against OpenSSL's secret. */
    unhex(A_PRIV, d);
    unhex(B_X, x);
    unhex(Z_AB, z);
    CHECK(crypto_ecdh_init(20) == NULL);
    forced_random = d;
    forced_random_len = sizeof(d);
    e1 = crypto_ecdh_init(19);
    CHECK(e1 != NULL);
    if (e1) {
        p1 = crypto_ecdh_get_pubkey(e1, 0);
        CHECK(p1 && wpabuf_len(p1) == 32 && os_memcmp(wpabuf_head(p1), der + 27, 32) == 0);
        wpabuf_free(p1);
        p1 = crypto_ecdh_get_pubkey(e1, 1);
        CHECK(p1 && wpabuf_len(p1) == 64 && os_memcmp(wpabuf_head(p1), der + 27, 64) == 0);
        wpabuf_free(p1);
        s1 = crypto_ecdh_set_peerkey(e1, 0, x, 32);
        CHECK(s1 && wpabuf_len(s1) == 32 && os_memcmp(wpabuf_head(s1), z, 32) == 0);
        wpabuf_free(s1);
        CHECK(crypto_ecdh_set_peerkey(e1, 0, x, 31) == NULL);
        os_memset(in, 0xff, 32);
        CHECK(crypto_ecdh_set_peerkey(e1, 0, in, 32) == NULL);
        /* x in 0..31: accepted exactly when x^3 - 3x + b is a square mod p. */
        for (unsigned v = 0; v < 32; v++) {
            struct crypto_bignum* bx = crypto_bignum_init_uint(v);
            struct crypto_bignum* y2 = crypto_ec_point_compute_y_sqr(ec, bx);
            int square = crypto_bignum_legendre(y2, crypto_ec_get_prime(ec)) == 1;
            os_memset(in, 0, 32);
            in[31] = (u8)v;
            s1 = crypto_ecdh_set_peerkey(e1, 0, in, 32);
            CHECK((s1 != NULL) == square);
            rejected += s1 == NULL;
            wpabuf_free(s1);
            crypto_bignum_deinit(bx, 0);
            crypto_bignum_deinit(y2, 0);
        }
        CHECK(rejected > 0 && rejected < 32);
        crypto_ecdh_deinit(e1);
    }
    /* Two random keys agree; with x || y as well. */
    for (int i = 0; i < 20; i++) {
        e1 = crypto_ecdh_init(19);
        e2 = crypto_ecdh_init(19);
        CHECK(e1 && e2);
        if (!e1 || !e2) {
            crypto_ecdh_deinit(e1);
            crypto_ecdh_deinit(e2);
            continue;
        }
        p1 = crypto_ecdh_get_pubkey(e1, i & 1);
        p2 = crypto_ecdh_get_pubkey(e2, i & 1);
        s1 = crypto_ecdh_set_peerkey(e1, i & 1, wpabuf_head(p2), wpabuf_len(p2));
        s2 = crypto_ecdh_set_peerkey(e2, i & 1, wpabuf_head(p1), wpabuf_len(p1));
        CHECK(s1 && s2 && wpabuf_len(s1) == 32 && wpabuf_len(s2) == 32 &&
              os_memcmp(wpabuf_head(s1), wpabuf_head(s2), 32) == 0);
        if (i & 1) {
            os_memcpy(out, wpabuf_head(p2), 64);
            out[63] ^= 1;
            CHECK(crypto_ecdh_set_peerkey(e1, 1, out, 64) == NULL);
        }
        wpabuf_free(p1);
        wpabuf_free(p2);
        wpabuf_free(s1);
        wpabuf_free(s2);
        crypto_ecdh_deinit(e1);
        crypto_ecdh_deinit(e2);
    }
    crypto_ec_deinit(ec);
}

int main(int argc, char** argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 200;
    u8 k1[SAE_PMK_LEN], k2[SAE_PMK_LEN];
    symmetric();
    sae_pk_owe();
    vectors();
    api_edges();
    for (int i = 0; i < rounds; i++) {
        int h2e = i & 1;
        CHECK(handshake("ESP32-WPA3", "ESP32-WPA3", h2e, k1, k2, 0) == 0 &&
              os_memcmp(k1, k2, SAE_PMK_LEN) == 0);
    }
    for (int h2e = 0; h2e < 2; h2e++) {
        CHECK(handshake("abcd1234", "wxyz5678", h2e, k1, k2, 0) != 0);
        for (int t = 1; t <= 3; t++)
            CHECK(handshake("same-pass", "same-pass", h2e, k1, k2, t) != 0);
    }
    printf("%s (%d failures, %d handshakes)\n", failures ? "FAILED" : "PASSED", failures, rounds);
    return failures != 0;
}
