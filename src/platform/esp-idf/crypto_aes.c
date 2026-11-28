/*
  crypto_aes.c - ESP-IDF hardware AES block provider for BearSSL

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file
 * @brief AES block runs in typical (non-DMA) mode under the ESP-IDF shared AES lock.
 * Engine ownership follows the SDK's own driver (mbedtls/port/aes): DMA-capable chips take
 * the esp_security lock and clock directly. The ESP32 block engine's SDK driver serializes
 * with a lock private to that driver, so while mbedTLS is linked (NEWMOTA_SDK_MBEDTLS) the
 * provider calls it; without mbedTLS nothing else uses the engine and the esp_security
 * lock and clock suffice.
 * The lock and key are taken once per run, then each block is written, run and read.
 * Mirrors the SDK's own single-block path, including its fault-injection check:
 * a result equal to the input means the engine did not run, and stops execution.
 */
#include "newmota/platform/crypto_aes.h"
#include "newmota_crypto.h"

#include <stdlib.h>
#include <string.h>

#include "esp_crypto_lock.h"
#include "esp_crypto_periph_clk.h"
#include "esp_timer.h"
#include "hal/aes_hal.h"
#include "hal/aes_ll.h"
#include "hal/aes_types.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if !SOC_AES_SUPPORT_AES_192
#error "BearSSL hardware AES requires support for 128, 192 and 256 bit keys"
#endif
#if !SOC_AES_SUPPORT_DMA && NEWMOTA_SDK_MBEDTLS
/* ESP32: the SDK driver's private spinlock (mbedtls/port/aes/block/esp_aes.c). */
void esp_aes_acquire_hardware(void);
void esp_aes_release_hardware(void);
#define AES_ACQUIRE() esp_aes_acquire_hardware()
#define AES_RELEASE() esp_aes_release_hardware()
#elif SOC_AES_SUPPORT_DMA && SOC_AES_CRYPTO_DMA
static void aes_acquire(void) {
    esp_crypto_dma_lock_acquire();
    esp_crypto_aes_enable_periph_clk(true);
}

static void aes_release(void) {
    esp_crypto_aes_enable_periph_clk(false);
    esp_crypto_dma_lock_release();
}

#define AES_ACQUIRE() aes_acquire()
#define AES_RELEASE() aes_release()
#else
static void aes_acquire(void) {
    esp_crypto_sha_aes_lock_acquire();
    esp_crypto_aes_enable_periph_clk(true);
}

static void aes_release(void) {
    esp_crypto_aes_enable_periph_clk(false);
    esp_crypto_sha_aes_lock_release();
}

#define AES_ACQUIRE() aes_acquire()
#define AES_RELEASE() aes_release()
#endif

void newmota_aes_blocks(const void* key, size_t key_len, int decrypt, const unsigned char* input,
    unsigned char* output, size_t num) {
    uint32_t aligned_key[8] = {0};
    uint32_t in[4], out[4];
    if (key_len != 16 && key_len != 24 && key_len != 32) {
        abort();
    }
    memcpy(aligned_key, key, key_len);
    AES_ACQUIRE();
#if SOC_AES_SUPPORT_DMA
    /* The SDK leaves DMA mode disabled after each transfer; never depend on it. */
    aes_ll_dma_enable(false);
#endif
    if (aes_hal_setkey((const uint8_t*)aligned_key, key_len,
            decrypt ? ESP_AES_DECRYPT : ESP_AES_ENCRYPT) != key_len) {
        abort();
    }
    for (; num; --num, input += 16, output += 16) {
        memcpy(in, input, sizeof(in));
        aes_ll_write_block((const uint8_t*)in);
        aes_ll_start_transform();
        const int64_t deadline = esp_timer_get_time() + 10000;
        while (aes_ll_get_state() != ESP_AES_STATE_IDLE) {
            /* Re-read after the deadline: preemption alone must not look like a stall. */
            if (esp_timer_get_time() >= deadline && aes_ll_get_state() != ESP_AES_STATE_IDLE) {
                abort();
            }
        }
        aes_ll_read_block(out);
        if (in[0] == out[0] && in[1] == out[1] && in[2] == out[2] && in[3] == out[3]) {
            abort();
        }
        memcpy(output, out, sizeof(out));
    }
    AES_RELEASE();
    nm_crypto_zero(aligned_key, sizeof(aligned_key));
    nm_crypto_zero(in, sizeof(in));
    nm_crypto_zero(out, sizeof(out));
}
