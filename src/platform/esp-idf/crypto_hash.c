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
 * clock on after a run and skips the enable while the clock and resets already match what
 * the pinned SDK's sha_ll_reset_register() leaves; see sha_clock_ready() and sha_clock_on().
 * Only resumable engines are supported. The original ESP32 cannot restore an arbitrary
 * chaining state and cannot meet BearSSL's hash contract this way.
 */
#include "newmota/platform/crypto_hash.h"
#include "newmota_features.h"

#include "newmota/platform/bearssl_inner.h"
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
#if CONFIG_IDF_TARGET_ESP32S3
#include "soc/system_reg.h"
#elif CONFIG_IDF_TARGET_ESP32P4
#include "soc/hp_sys_clkrst_reg.h"
#endif

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
 * @brief Whether the SHA clock is on and the engine out of reset, read without the RCC lock.
 * Mirrors the pinned SDK's sha_ll_reset_register(): besides the SHA reset, every reset it
 * clears because it also holds SHA (DS, HMAC, ECDSA, P4's parent crypto reset) must be clear.
 * A CPU software reset on the S3 keeps the SHA clock on (the bootloader enabled it) but app
 * startup asserts the DS and HMAC resets; checking the SHA bits alone then skipped the enable
 * and the first block stalled. Targets without a register read here report false and keep
 * the SDK's per-run enable and reset.
 */
static inline bool sha_clock_ready(void) {
#if CONFIG_SECURE_ENABLE_TEE
    return false; /* the TEE owns the clock registers */
#elif CONFIG_IDF_TARGET_ESP32S3
    /* One read of perip_rst_en1 for all three resets. */
    return SYSTEM.perip_clk_en1.crypto_sha_clk_en &&
           (SYSTEM.perip_rst_en1.val &
               (SYSTEM_CRYPTO_SHA_RST | SYSTEM_CRYPTO_DS_RST | SYSTEM_CRYPTO_HMAC_RST)) == 0;
#elif CONFIG_IDF_TARGET_ESP32C6
    return PCR.sha_conf.sha_clk_en && !PCR.sha_conf.sha_rst_en && !PCR.ds_conf.ds_rst_en &&
           !PCR.hmac_conf.hmac_rst_en;
#elif CONFIG_IDF_TARGET_ESP32C5 || CONFIG_IDF_TARGET_ESP32H2
    return PCR.sha_conf.sha_clk_en && !PCR.sha_conf.sha_rst_en && !PCR.ds_conf.ds_rst_en &&
           !PCR.hmac_conf.hmac_rst_en && !PCR.ecdsa_conf.ecdsa_rst_en;
#elif CONFIG_IDF_TARGET_ESP32C61
    return PCR.sha_conf.sha_clk_en && !PCR.sha_conf.sha_rst_en && !PCR.ecdsa_conf.ecdsa_rst_en;
#elif CONFIG_IDF_TARGET_ESP32P4
    /* One read of hp_rst_en2 for all five resets. */
    return HP_SYS_CLKRST.peri_clk_ctrl25.reg_crypto_sha_clk_en &&
           (HP_SYS_CLKRST.hp_rst_en2.val &
               (HP_SYS_CLKRST_REG_RST_EN_SHA | HP_SYS_CLKRST_REG_RST_EN_CRYPTO |
                   HP_SYS_CLKRST_REG_RST_EN_DS | HP_SYS_CLKRST_REG_RST_EN_HMAC |
                   HP_SYS_CLKRST_REG_RST_EN_ECDSA)) == 0;
#else
    return false;
#endif
}

/**
 * @brief Turn the SHA clock on (with a module reset) unless sha_clock_ready() says the
 * engine is clocked and out of every reset that holds it.
 * Called with the SHA lock held. The clock stays on after the run: every other SDK
 * user (the mbedTLS port, esp_hmac, esp_ds) turns it on itself and off again under
 * the same lock, so a later run sees the cleared bit here and turns it on again.
 * A reset left asserted by startup, such as DS/HMAC after a software reset on the S3,
 * fails the predicate the same way and takes the SDK enable, which clears it.
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

#if USE_CRYPTO_HW_SHA1 && USE_WIFI_WPA3_BEARSSL
/*
 * PBKDF2-HMAC-SHA1 for the supplicant's PSK (hostap pbkdf2_sha1()). The link wraps the
 * symbol (main/CMakeLists.txt), so wpa.c, ap_config.c and the crypto_ops table call this
 * instead of hostap's sha1-pbkdf2.c, which rehashes both 64-byte key pads with two
 * engine runs each on every iteration. Here the pads are hashed once into midstates
 * and each HMAC is one block from the inner and one from the outer midstate, with the
 * engine held across PBKDF2_LOCK_ITERATIONS iterations. All words stay in engine byte
 * order: a digest read from the engine is already the next message's bytes.
 */
/* 64 iterations hold the lock for under 1 ms (S3, C5); the re-take costs about 1 %. */
#define PBKDF2_LOCK_ITERATIONS 64
#define PBKDF2_MAX_SSID 51 /* SSID, count, 0x80 and the length still fit one block */

int __real_pbkdf2_sha1(const char* passphrase, const unsigned char* ssid, size_t ssid_len,
    int iterations, unsigned char* buf, size_t buflen);
int __wrap_pbkdf2_sha1(const char* passphrase, const unsigned char* ssid, size_t ssid_len,
    int iterations, unsigned char* buf, size_t buflen);

static void sha1_engine_take(void) {
    esp_crypto_sha_aes_lock_acquire();
    sha_clock_on();
    wait_idle();
    sha_hal_set_mode(SHA1);
}

/** @brief One SHA-1 block continued from a midstate; engine held and in SHA-1 mode. */
static void sha1_block_from(const uint32_t* mid, const uint32_t* block, uint32_t* out) {
    sha_hal_write_digest(SHA1, (void*)mid);
    sha_hal_hash_block(SHA1, block, 16, false);
    wait_idle();
    sha_hal_read_digest(SHA1, out);
}

/** @brief Big-endian message bit count in the last eight bytes of a final block. */
static void put_bit_length(uint32_t* block, size_t bytes) {
    unsigned char* b = (unsigned char*)block;
    const uint64_t bits = (uint64_t)bytes * 8;
    for (int i = 0; i < 8; ++i) {
        b[63 - i] = (unsigned char)(bits >> (8 * i));
    }
}

int __wrap_pbkdf2_sha1(const char* passphrase, const unsigned char* ssid, size_t ssid_len,
    int iterations, unsigned char* buf, size_t buflen) {
    if (iterations < 1 || ssid_len > PBKDF2_MAX_SSID) {
        return __real_pbkdf2_sha1(passphrase, ssid, ssid_len, iterations, buf, buflen);
    }
    static const uint32_t sha1_iv[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    uint32_t key[16] = {0}, iv[5], ipad[5], opad[5], block[16], u[5], t[5];
    size_t key_len = strlen(passphrase);
    if (key_len > sizeof key) {
        br_sha1_context ctx;
        br_sha1_init(&ctx);
        br_sha1_update(&ctx, passphrase, key_len);
        br_sha1_out(&ctx, key);
        nm_crypto_zero(&ctx, sizeof ctx);
    } else {
        memcpy(key, passphrase, key_len);
    }
    swap32(iv, sha1_iv, 5);

    sha1_engine_take();
    for (size_t i = 0; i < 16; ++i) {
        block[i] = key[i] ^ 0x36363636u;
    }
    sha1_block_from(iv, block, ipad);
    for (size_t i = 0; i < 16; ++i) {
        block[i] = key[i] ^ 0x5c5c5c5cu;
    }
    sha1_block_from(iv, block, opad);

    unsigned char* pos = buf;
    size_t left = buflen;
    for (uint32_t count = 1; left > 0; ++count) {
        /* U_1 = HMAC(passphrase, ssid || INT(count)) */
        unsigned char* b = (unsigned char*)block;
        memset(block, 0, sizeof block);
        memcpy(b, ssid, ssid_len);
        b[ssid_len] = (unsigned char)(count >> 24);
        b[ssid_len + 1] = (unsigned char)(count >> 16);
        b[ssid_len + 2] = (unsigned char)(count >> 8);
        b[ssid_len + 3] = (unsigned char)count;
        b[ssid_len + 4] = 0x80;
        put_bit_length(block, 64 + ssid_len + 4);
        sha1_block_from(ipad, block, u);
        /* Every later block is a 20-byte message after a 64-byte pad. */
        memset(block, 0, sizeof block);
        b[20] = 0x80;
        put_bit_length(block, 64 + 20);
        memcpy(block, u, 20);
        sha1_block_from(opad, block, u);
        memcpy(t, u, sizeof t);
        for (int j = 1; j < iterations; ++j) {
            if (j % PBKDF2_LOCK_ITERATIONS == 0) {
                /* Let other engine users in; they may change the mode or the clock. */
                esp_crypto_sha_aes_lock_release();
                sha1_engine_take();
            }
            memcpy(block, u, 20);
            sha1_block_from(ipad, block, u);
            memcpy(block, u, 20);
            sha1_block_from(opad, block, u);
            for (size_t k = 0; k < 5; ++k) {
                t[k] ^= u[k];
            }
        }
        const size_t n = left < sizeof t ? left : sizeof t;
        memcpy(pos, t, n);
        pos += n;
        left -= n;
    }
    esp_crypto_sha_aes_lock_release();
    nm_crypto_zero(key, sizeof key);
    nm_crypto_zero(ipad, sizeof ipad);
    nm_crypto_zero(opad, sizeof opad);
    nm_crypto_zero(block, sizeof block);
    nm_crypto_zero(u, sizeof u);
    nm_crypto_zero(t, sizeof t);
    return 0;
}
#endif
