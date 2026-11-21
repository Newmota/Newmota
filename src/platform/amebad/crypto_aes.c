/*
  crypto_aes.c - AmebaD hardware AES blocks for BearSSL

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file
 * @brief Use the SDK crypto owner and full AES-128/192/256 ECB engine.
 * Aligned static buffers keep DMA away from user memory and survive engine errors.
 * The HP bootloader initializes the engine in ROM-owned state before the application
 * starts; an uninitialized engine stops execution. Each call replaces its transient
 * key and runs up to NEWMOTA_AES_RUN_BLOCKS blocks per engine operation; contexts contain
 * no pointers into the shared SDK adapter.
 */
#include "newmota/platform/crypto_aes.h"
#include "newmota_crypto.h"
#include "ameba_soc.h"
#include "device_lock.h"
#include <stdlib.h>
#include <string.h>

static unsigned char raw_key[32] __attribute__((aligned(32)));
static unsigned char source[NEWMOTA_AES_RUN_BLOCKS * 16] __attribute__((aligned(32)));
static unsigned char result[NEWMOTA_AES_RUN_BLOCKS * 16] __attribute__((aligned(32)));

void newmota_aes_blocks(const void* key, size_t key_len, int decrypt, const unsigned char* input,
    unsigned char* output, size_t num) {
    if (key_len != 16 && key_len != 24 && key_len != 32)
        abort();
    device_mutex_lock(RT_DEV_LOCK_CRYPTO);
    if (!crypto_engine.isInit)
        abort();
    memcpy(raw_key, key, key_len);
    if (rtl_crypto_aes_ecb_init(raw_key, key_len) != 0)
        abort();
    /* Wipe only what was used: single-block chained modes call this once per block. */
    size_t used = num < NEWMOTA_AES_RUN_BLOCKS ? num * 16 : sizeof source;
    while (num) {
        size_t run = num < NEWMOTA_AES_RUN_BLOCKS ? num : NEWMOTA_AES_RUN_BLOCKS;
        memcpy(source, input, run * 16);
        int status = decrypt ? rtl_crypto_aes_ecb_decrypt(source, run * 16, NULL, 0, result)
                             : rtl_crypto_aes_ecb_encrypt(source, run * 16, NULL, 0, result);
        /* The SDK can return while DMA is still active on error. Do not unlock or
           release any buffer before stopping execution. */
        if (status != 0)
            abort();
        memcpy(output, result, run * 16);
        input += run * 16;
        output += run * 16;
        num -= run;
    }
    nm_crypto_zero(raw_key, key_len);
    nm_crypto_zero(source, used);
    nm_crypto_zero(result, used);
    device_mutex_unlock(RT_DEV_LOCK_CRYPTO);
}
