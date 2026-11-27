/*
  crypto_hash.c - ESP-IDF hardware SHA compression for BearSSL

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file
 * @brief SHA compression using the SDK's shared crypto lock and clock control.
 * Calls esp_security and the SHA HAL directly, as the SDK's own engine driver
 * (mbedtls/port/sha/core) does, so it coexists with that driver when mbedTLS is linked
 * and needs no mbedTLS component when it is not. Unlike that driver it leaves the SHA
 * clock on after a run; see sha_clock_on(). Only resumable engines are supported. The original ESP32 cannot restore an
 * arbitrary chaining state and cannot meet BearSSL's hash contract this way.
 */
#include "newmota/platform/crypto_hash.h"
#include "newmota_features.h"

#include "inner.h"
#include "newmota_crypto.h"
#include "esp_crypto_lock.h"
#include "esp_crypto_periph_clk.h"
#include "esp_timer.h"
#include "hal/sha_hal.h"
#include "hal/sha_ll.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if !SOC_SHA_SUPPORT_RESUME
#error "BearSSL hardware SHA requires arbitrary digest-state restoration"
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

/** @brief Busy polls before the timer-checked wait; one block finishes within a few µs. */
#define SPIN_POLLS 256

/**
 * @brief Whether the SHA clock is on and the module out of reset, read without the RCC lock.
 * Targets without a register read here report false and keep the SDK's per-run
 * enable and reset.
 */
static inline bool sha_clock_ready(void) {
#if CONFIG_SECURE_ENABLE_TEE
    return false; /* the TEE owns the clock registers */
#elif CONFIG_IDF_TARGET_ESP32S3
    return SYSTEM.perip_clk_en1.crypto_sha_clk_en && !SYSTEM.perip_rst_en1.crypto_sha_rst;
#elif CONFIG_IDF_TARGET_ESP32C5 || CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32C61 || \
    CONFIG_IDF_TARGET_ESP32H2
    return PCR.sha_conf.sha_clk_en && !PCR.sha_conf.sha_rst_en;
#elif CONFIG_IDF_TARGET_ESP32P4
    return HP_SYS_CLKRST.peri_clk_ctrl25.reg_crypto_sha_clk_en &&
           !HP_SYS_CLKRST.hp_rst_en2.reg_rst_en_sha && !HP_SYS_CLKRST.hp_rst_en2.reg_rst_en_crypto;
#else
    return false;
#endif
}

/**
 * @brief Turn the SHA clock on (with a module reset) only when it is off.
 * Called with the SHA lock held. The clock stays on after the run: every other SDK
 * user (the mbedTLS port, esp_hmac, esp_ds) turns it on itself and off again under
 * the same lock, so a later run sees the cleared bit here and turns it on again.
 * Every run writes the mode and the full state, so it needs no reset.
 */
static inline void sha_clock_on(void) {
    if (!sha_clock_ready()) {
        esp_crypto_sha_enable_periph_clk(true);
    }
}

/** @brief Fail closed on a stalled engine, before returning or releasing ownership. */
static void wait_idle(void) {
    for (unsigned i = 0; i < SPIN_POLLS; ++i) {
        if (!sha_ll_busy()) {
            return;
        }
    }
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
    esp_crypto_sha_aes_lock_acquire();
    sha_clock_on();
    wait_idle();
    sha_hal_set_mode(mode);
    sha_hal_write_digest(mode, state);
    for (size_t i = 0; i < num; ++i, buf += block_len) {
#if CONFIG_IDF_TARGET_ARCH_RISCV
        /* Tasmota's driver saw RISC-V engines overrun past eight continued blocks;
           checkpoint the state the same way. */
        if (i > 0 && (i & 7) == 0) {
            sha_hal_read_digest(mode, state);
            sha_hal_write_digest(mode, state);
        }
#endif
        memcpy(block, buf, block_len);
        sha_hal_hash_block(mode, block, block_len / 4, false);
        wait_idle();
    }
    sha_hal_read_digest(mode, state);
    esp_crypto_sha_aes_lock_release();
    nm_crypto_zero(block, block_len);
}

/**
 * @brief Byte swap written as shifts, not __builtin_bswap32.
 * RISC-V without Zbb has no swap instruction, and GCC turns the builtin into a ROM
 * __bswapsi2 call per word; the shift form stays inline.
 */
static inline uint32_t swap_word(uint32_t x) {
    const uint32_t t = (x << 16) | (x >> 16);
    return ((t & 0x00ff00ffu) << 8) | ((t >> 8) & 0x00ff00ffu);
}

/**
 * @brief Native words to and from the engine's big-endian byte words.
 * At the build's -Os, Xtensa GCC also calls ROM __bswapsi2 for the shift form;
 * at O2 it emits four inline instructions per word.
 */
__attribute__((optimize("O2"))) static void swap32(uint32_t* dst, const uint32_t* src, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        dst[i] = swap_word(src[i]);
    }
}

#if USE_CRYPTO_HW_SHA1
void newmota_sha1_blocks(const unsigned char* buf, size_t num, uint32_t* val) {
    uint32_t state[5];
    swap32(state, val, 5);
    compress(SHA1, buf, num, state, 64);
    swap32(val, state, 5);
    nm_crypto_zero(state, sizeof state);
}
#endif

#if USE_CRYPTO_HW_SHA256
void newmota_sha256_blocks(const unsigned char* buf, size_t num, uint32_t* val) {
    uint32_t state[8];
    swap32(state, val, 8);
    compress(SHA2_256, buf, num, state, 64);
    swap32(val, state, 8);
    nm_crypto_zero(state, sizeof state);
}
#endif

#if USE_CRYPTO_HW_SHA512
void newmota_sha512_blocks(const unsigned char* buf, size_t num, uint64_t* val) {
    uint32_t state[16];
    uint32_t words[16];
    for (size_t i = 0; i < 8; ++i) {
        words[2 * i] = (uint32_t)(val[i] >> 32);
        words[2 * i + 1] = (uint32_t)val[i];
    }
    swap32(state, words, 16);
    compress(SHA2_512, buf, num, state, 128);
    swap32(words, state, 16);
    for (size_t i = 0; i < 8; ++i) {
        val[i] = ((uint64_t)words[2 * i] << 32) | words[2 * i + 1];
    }
    nm_crypto_zero(words, sizeof words);
    nm_crypto_zero(state, sizeof state);
}
#endif
