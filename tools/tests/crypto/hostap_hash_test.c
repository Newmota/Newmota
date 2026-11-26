/*
  hostap_hash_test.c - hostap HMAC, PBKDF2-SHA1 and SHA-384/512 on BearSSL

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file
 * @brief Known answers for src/platform/crypto/hostap_hash_bearssl.c, the hash functions the
 * prebuilt Beken supplicant takes from its backend: HMAC-MD5 and HMAC-SHA1 (RFC 2202),
 * HMAC-SHA-256/384/512 (RFC 4231, including a key longer than the block), SHA-384/512
 * ("abc", FIPS 180-2), PBKDF2-SHA1 (RFC 6070 and IEEE Std 802.11 Annex J.4), split vectors
 * against single buffers, and the rejected iteration count. Compiled against the pinned
 * ESP-IDF hostap headers, whose prototypes match the BDK's. Exit status is the failure count.
 */
#include "utils/includes.h"
#include "utils/common.h"
#include "crypto/md5.h"
#include "crypto/sha1.h"
#include "crypto/sha256.h"
#include <stdio.h>
#include <string.h>

int hmac_md5(const u8* key, size_t key_len, const u8* data, size_t data_len, u8* mac);
int sha384_vector(size_t num_elem, const u8* addr[], const size_t* len, u8* mac);
int sha512_vector(size_t num_elem, const u8* addr[], const size_t* len, u8* mac);
int hmac_sha384(const u8* key, size_t key_len, const u8* data, size_t data_len, u8* mac);
int hmac_sha512(const u8* key, size_t key_len, const u8* data, size_t data_len, u8* mac);

static int failures;

static void hex(const char* text, u8* out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned int byte;
        sscanf(text + 2 * i, "%2x", &byte);
        out[i] = (u8)byte;
    }
}

static void expect(const char* name, const u8* got, const char* want_hex) {
    u8 want[64];
    size_t len = strlen(want_hex) / 2;
    hex(want_hex, want, len);
    if (memcmp(got, want, len) != 0) {
        failures++;
        fprintf(stderr, "FAIL %s\n", name);
    }
}

int main(void) {
    u8 key20[20], key131[131], mac[64], mac2[64];
    const u8* hi = (const u8*)"Hi There";
    memset(key20, 0x0b, sizeof(key20));
    memset(key131, 0xaa, sizeof(key131));

    hmac_md5(key20, 16, hi, 8, mac);
    expect("hmac_md5 rfc2202-1", mac, "9294727a3638bb1c13f48ef8158bfc9d");
    hmac_sha1(key20, 20, hi, 8, mac);
    expect("hmac_sha1 rfc2202-1", mac, "b617318655057264e28bc0b6fb378c8ef146be00");
    hmac_sha1((const u8*)"Jefe", 4, (const u8*)"what do ya want for nothing?", 28, mac);
    expect("hmac_sha1 rfc2202-2", mac, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
    hmac_sha256(key20, 20, hi, 8, mac);
    expect("hmac_sha256 rfc4231-1", mac,
           "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    hmac_sha384(key20, 20, hi, 8, mac);
    expect("hmac_sha384 rfc4231-1", mac,
           "afd03944d84895626b0825f4ab46907f15f9dadbe4101ec682aa034c7cebc59c"
           "faea9ea9076ede7f4af152e8b2fa9cb6");
    hmac_sha512(key20, 20, hi, 8, mac);
    expect("hmac_sha512 rfc4231-1", mac,
           "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cde"
           "daa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854");
    const char* big = "Test Using Larger Than Block-Size Key - Hash Key First";
    hmac_sha256(key131, sizeof(key131), (const u8*)big, strlen(big), mac);
    expect("hmac_sha256 rfc4231-6", mac,
           "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

    /* Split input equals the single buffer. */
    const u8* parts[3] = {(const u8*)"what do ya", (const u8*)" want for ",
                          (const u8*)"nothing?"};
    size_t lens[3] = {10, 10, 8};
    hmac_sha1_vector((const u8*)"Jefe", 4, 3, parts, lens, mac);
    expect("hmac_sha1_vector split", mac, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
    hmac_sha256_vector((const u8*)"Jefe", 4, 3, parts, lens, mac);
    hmac_sha256((const u8*)"Jefe", 4, (const u8*)"what do ya want for nothing?", 28, mac2);
    if (memcmp(mac, mac2, 32) != 0) {
        failures++;
        fprintf(stderr, "FAIL hmac_sha256_vector split\n");
    }

    const u8* abc = (const u8*)"abc";
    size_t three = 3;
    sha384_vector(1, &abc, &three, mac);
    expect("sha384 abc", mac,
           "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
           "8086072ba1e7cc2358baeca134c825a7");
    sha512_vector(1, &abc, &three, mac);
    expect("sha512 abc", mac,
           "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
           "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");

    pbkdf2_sha1("password", (const u8*)"salt", 4, 1, mac, 20);
    expect("pbkdf2 rfc6070-1", mac, "0c60c80f961f0e71f3a9b524af6012062fe037a6");
    pbkdf2_sha1("password", (const u8*)"salt", 4, 2, mac, 20);
    expect("pbkdf2 rfc6070-2", mac, "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957");
    pbkdf2_sha1("passwordPASSWORDpassword", (const u8*)"saltSALTsaltSALTsaltSALTsaltSALTsalt",
                36, 4096, mac, 25);
    expect("pbkdf2 rfc6070-5", mac, "3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038");
    memset(mac, 0x5a, sizeof(mac));
    pbkdf2_sha1("password", (const u8*)"IEEE", 4, 4096, mac, 32);
    expect("pbkdf2 802.11 psk", mac,
           "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e");
    if (mac[32] != 0x5a) {
        failures++;
        fprintf(stderr, "FAIL pbkdf2 wrote past buflen\n");
    }
    if (pbkdf2_sha1("password", (const u8*)"IEEE", 4, 0, mac, 32) != -1) {
        failures++;
        fprintf(stderr, "FAIL pbkdf2 accepted 0 iterations\n");
    }

    printf("hostap hash test: %d failure(s)\n", failures);
    return failures > 100 ? 100 : failures;
}
