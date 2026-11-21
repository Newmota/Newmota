/* Newmota portable BearSSL extensions. Copyright (C) 2026 Newmota contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NEWMOTA_CRYPTO_EXTENSIONS_H
#define NEWMOTA_CRYPTO_EXTENSIONS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Volatile stores keep cleanup observable to the compiler. */
void nm_crypto_zero(void *data, size_t length);
int nm_crypto_equal(const void *a, const void *b, size_t length);

/* RFC 8018: nonzero iteration count; output <= (2^32-1)*32 bytes.
 * Output must not overlap password or salt. Failure does not modify output. */
int nm_pbkdf2_sha256(const void *password, size_t password_len,
    const void *salt, size_t salt_len, uint32_t iterations,
    void *output, size_t output_len);

/* RFC 7693 sequential BLAKE2s, including keyed and truncated output.
 * Each context owns its buffer. out is repeatable and does not consume context. */
typedef struct {
    uint32_t h[8];
    uint64_t count;
    unsigned char buffer[64];
    size_t buffered;
    size_t output_len;
} nm_blake2s_context;
int nm_blake2s_init(nm_blake2s_context *ctx, size_t output_len,
    const void *key, size_t key_len);
int nm_blake2s_update(nm_blake2s_context *ctx, const void *data, size_t length);
int nm_blake2s_out(const nm_blake2s_context *ctx, void *output);
int nm_hmac_blake2s(const void *key, size_t key_len,
    const void *data, size_t length, void *output);

/* RFC 8032 Ed25519 (pure mode). Fixed arrays: seed/public 32, secret/signature 64.
 * Secret key is seed || public key, matching the pinned Tasmota Berry API.
 * Verification requires canonical encodings and prime-subgroup points. */
int nm_ed25519_secret_key(const void *seed, void *secret_key);
int nm_ed25519_sign(const void *secret_key, const void *message,
    size_t message_len, void *signature);
int nm_ed25519_verify(const void *public_key, const void *message,
    size_t message_len, const void *signature);

/* HChaCha20 draft-irtf-cfrg-xchacha section 2.2; fixed lengths 32/16/32. */
int nm_hchacha20(const void *key, const void *nonce, void *output);
/* Raw Poly1305: one-time key is 32 bytes, tag 16 bytes. Never reuse a key. */
int nm_poly1305(const void *key, const void *data, size_t length, void *tag);
/* In-place XChaCha20-Poly1305. Fixed lengths: key 32, nonce 24, tag 16.
 * Decryption authenticates before modifying the payload. Returns 0 on failure. */
int nm_xchacha20_poly1305(const void *key, const void *nonce, void *data,
    size_t length, const void *aad, size_t aad_len, void *tag, int encrypt);
#ifdef __cplusplus
}
#endif
#endif
