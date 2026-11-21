/*
  crypto_hash.c - ESP-IDF hardware SHA compression for BearSSL

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file
 * @brief SHA compression using the SDK's shared crypto lock and clock lifecycle.
 * Only resumable engines are supported. The original ESP32 cannot restore an
 * arbitrary chaining state and cannot meet BearSSL's hash contract this way.
 */
#include "newmota/platform/crypto_hash.h"
#include "newmota_features.h"

#include "inner.h"
#include "newmota_crypto.h"
#include "esp_timer.h"
#include "hal/sha_ll.h"
#include "sdkconfig.h"
#include "sha/sha_core.h"
#include "soc/soc_caps.h"
#include <stdlib.h>

#if !SOC_SHA_SUPPORT_RESUME
#error "BearSSL hardware SHA requires arbitrary digest-state restoration"
#endif
#if !CONFIG_MBEDTLS_HARDWARE_SHA
#error "BearSSL hardware SHA requires the SDK hardware SHA owner"
#endif
#if USE_CRYPTO_HW_SHA1 && !SOC_SHA_SUPPORT_SHA1
#error "Selected target has no SHA-1 engine"
#endif
#if USE_CRYPTO_HW_SHA256 && !SOC_SHA_SUPPORT_SHA256
#error "Selected target has no SHA-256 engine"
#endif
#if USE_CRYPTO_HW_SHA512 && !SOC_SHA_SUPPORT_SHA512
#error "Selected target has no SHA-512 engine"
#endif

/** @brief Fail closed on a stalled engine, before returning or releasing ownership. */
static void wait_idle(void) {
    const int64_t deadline = esp_timer_get_time() + 10000;
    while (sha_ll_busy()) {
        /* Re-read after the deadline: preemption alone must not look like a stall. */
        if (esp_timer_get_time() >= deadline && sha_ll_busy()) {
            abort();
        }
    }
}

/**
 * @brief Compress a run of blocks through an aligned local message buffer, no DMA.
 * The engine is acquired and the state loaded once per run, not per block.
 * SDK digest states are big-endian bytes; BearSSL stores native-word integers.
 * Always continue from the supplied state, including the first block, so SHA-224
 * and SHA-384 use their own IV with the SHA-256 and SHA-512 compression engines.
 */
static void compress(
    esp_sha_type mode, const unsigned char* buf, size_t num, uint32_t* state, size_t block_len) {
    uint32_t block[32];
    esp_sha_acquire_hardware();
    wait_idle();
    esp_sha_set_mode(mode);
    esp_sha_write_digest_state(mode, state);
    for (size_t i = 0; i < num; ++i, buf += block_len) {
#if CONFIG_IDF_TARGET_ARCH_RISCV
        /* Tasmota's driver saw RISC-V engines overrun past eight continued blocks;
           checkpoint the state the same way. */
        if (i > 0 && (i & 7) == 0) {
            esp_sha_read_digest_state(mode, state);
            esp_sha_write_digest_state(mode, state);
        }
#endif
        memcpy(block, buf, block_len);
        esp_sha_block(mode, block, false);
        wait_idle();
    }
    esp_sha_read_digest_state(mode, state);
    esp_sha_release_hardware();
    nm_crypto_zero(block, block_len);
}

#if USE_CRYPTO_HW_SHA1
void newmota_sha1_blocks(const unsigned char* buf, size_t num, uint32_t* val) {
    uint32_t state[5];
    br_range_enc32be(state, val, 5);
    compress(SHA1, buf, num, state, 64);
    br_range_dec32be(val, 5, state);
    nm_crypto_zero(state, sizeof state);
}
#endif

#if USE_CRYPTO_HW_SHA256
void newmota_sha256_blocks(const unsigned char* buf, size_t num, uint32_t* val) {
    uint32_t state[8];
    br_range_enc32be(state, val, 8);
    compress(SHA2_256, buf, num, state, 64);
    br_range_dec32be(val, 8, state);
    nm_crypto_zero(state, sizeof state);
}
#endif

#if USE_CRYPTO_HW_SHA512
void newmota_sha512_blocks(const unsigned char* buf, size_t num, uint64_t* val) {
    uint32_t state[16];
    br_range_enc64be(state, val, 8);
    compress(SHA2_512, buf, num, state, 128);
    br_range_dec64be(val, 8, state);
    nm_crypto_zero(state, sizeof state);
}
#endif
