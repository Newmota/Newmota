/*
  hostap_bignum_fuzz.c - line-oriented driver for the hostap bignum backend

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file
 * @brief Reads "op a b m" lines (hex operands) and prints "rc result" per line, so that
 * test_hostap_bignum.py can compare every crypto_bignum_* operation with Python integers.
 */
#include "utils/includes.h"
#include "utils/common.h"
#include "crypto/crypto.h"
#include <stdio.h>
#include <stdlib.h>
const wifi_osi_funcs_t *wifi_funcs;
/* /dev/urandom rather than arc4random, which glibc has only from 2.36. */
int os_get_random(unsigned char *buf, size_t len) {
    static FILE *f;
    if (f == NULL) f = fopen("/dev/urandom", "rb");
    return f != NULL && fread(buf, 1, len, f) == len ? 0 : -1;
}
static struct crypto_bignum *rd(const char *h) {
    u8 b[300]; size_t n = strlen(h) / 2;
    for (size_t i = 0; i < n; i++) sscanf(h + 2 * i, "%2hhx", &b[i]);
    return crypto_bignum_init_set(b, n);
}
static void pr(int rc, const struct crypto_bignum *x) {
    u8 b[300]; int n = rc < 0 ? -1 : crypto_bignum_to_bin(x, b, sizeof(b), 0);
    printf("%d ", rc);
    if (rc >= 0) { if (n == 0) printf("00"); for (int i = 0; i < n; i++) printf("%02x", b[i]); }
    printf("\n");
}
int main(void) {
    char op[16], ha[300], hb[300], hm[300];
    while (scanf("%15s %299s %299s %299s", op, ha, hb, hm) == 4) {
        struct crypto_bignum *a = rd(ha), *b = rd(hb), *m = rd(hm), *r = crypto_bignum_init();
        int rc;
        if (!strcmp(op, "add")) rc = crypto_bignum_add(a, b, r);
        else if (!strcmp(op, "sub")) rc = crypto_bignum_sub(a, b, r);
        else if (!strcmp(op, "mod")) rc = crypto_bignum_mod(a, m, r);
        else if (!strcmp(op, "div")) rc = crypto_bignum_div(a, m, r);
        else if (!strcmp(op, "mulmod")) rc = crypto_bignum_mulmod(a, b, m, r);
        else if (!strcmp(op, "addmod")) rc = crypto_bignum_addmod(a, b, m, r);
        else if (!strcmp(op, "sqrmod")) rc = crypto_bignum_sqrmod(a, m, r);
        else if (!strcmp(op, "exptmod")) rc = crypto_bignum_exptmod(a, b, m, r);
        else if (!strcmp(op, "inverse")) rc = crypto_bignum_inverse(a, m, r);
        else if (!strcmp(op, "rshift")) rc = crypto_bignum_rshift(a, (int)strtol(hb, NULL, 16), r);
        else if (!strcmp(op, "legendre")) { printf("%d\n", crypto_bignum_legendre(a, m)); goto next; }
        else if (!strcmp(op, "cmp")) { printf("%d\n", crypto_bignum_cmp(a, b)); goto next; }
        else if (!strcmp(op, "bits")) { printf("%d\n", crypto_bignum_bits(a)); goto next; }
        else { printf("?\n"); goto next; }
        pr(rc, r);
    next:
        crypto_bignum_deinit(a, 1); crypto_bignum_deinit(b, 1); crypto_bignum_deinit(m, 1); crypto_bignum_deinit(r, 1);
    }
    return 0;
}
