/* Copyright (C) 2026 Newmota contributors. SPDX-License-Identifier: GPL-3.0-or-later */
#include "newmota_crypto.h"
#include <string.h>
/* Word-wide memset; the barrier makes the compiler assume the zeroed bytes are read. */
void nm_crypto_zero(void *data, size_t length) {
    memset(data, 0, length);
    __asm__ __volatile__("" : : "r"(data) : "memory");
}
int nm_crypto_equal(const void *a, const void *b, size_t length) {
    const unsigned char *x = a, *y = b;
    unsigned char diff = 0;
    while (length-- != 0) diff |= *x++ ^ *y++;
    return diff == 0;
}
