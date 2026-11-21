/*
  crypto_aes.c - Bouffalo hardware AES blocks for BearSSL

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file
 * @brief Complete AES-128/192/256 using the SDK's AES mutex and DMA link engine.
 * Static DMA storage remains owned on failure; abort never releases a live engine.
 * The BL602 Wi-Fi crypto adapter drives this engine inside a critical section without
 * the mutex, so engine use is also a critical section, one per chunk of up to
 * NEWMOTA_AES_RUN_BLOCKS blocks; the mutex is taken once per run. The timeouts read mtime
 * directly.
 */
#include "newmota/platform/crypto_aes.h"
#include "newmota_crypto.h"
#include "FreeRTOS.h"
#include "task.h"
#include "bflb_sec_aes.h"
#include "bflb_sec_mutex.h"
#include "hardware/sec_eng_reg.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(BL618DG)
#include "bl618dg_glb.h"
#endif
#if defined(BL602)
#define DMA_STORAGE __attribute__((section(".wifi_ram"), aligned(32)))
#else
#define DMA_STORAGE ATTR_NOCACHE_NOINIT_RAM_SECTION __attribute__((aligned(32)))
#endif
static DMA_STORAGE struct bflb_aes_link_s descriptor;
static DMA_STORAGE unsigned char source[NEWMOTA_AES_RUN_BLOCKS * 16];
static DMA_STORAGE unsigned char result[NEWMOTA_AES_RUN_BLOCKS * 16];

/** @brief Report failure without releasing a live engine or exposing key or buffer data. */
static _Noreturn void aes_fail(const char* reason, struct bflb_device_s* dev) {
    printf("Bouffalo AES failure: %s\r\n", reason);
    if (dev) {
        printf("AES ctrl=%08x status=%08x link=%08x prot=%08x\r\n",
            (unsigned)getreg32(dev->reg_base + SEC_ENG_SE_AES_0_CTRL_OFFSET),
            (unsigned)getreg32(dev->reg_base + SEC_ENG_SE_AES_0_STATUS_OFFSET),
            (unsigned)getreg32(dev->reg_base + SEC_ENG_SE_AES_0_LINK_OFFSET),
            (unsigned)getreg32(dev->reg_base + SEC_ENG_SE_CTRL_PROT_RD_OFFSET));
    }
    abort();
}

/** @brief Run up to NEWMOTA_AES_RUN_BLOCKS blocks in one critical section and link update. */
static void run_chunk(struct bflb_device_s* dev, const void* key, size_t key_len, int decrypt,
    const unsigned char* input, unsigned char* output, size_t num) {
    taskENTER_CRITICAL();
#if defined(BL602)
    /* BL602 readback (0xF00) has AES PROT_EN/ID0/ID1 at bits 4/5/6; the writable
       register has them at bits 0/1/2 (bl_iot_sdk@14ebffe9, BL602 sec_eng_reg.h).
       The SDK helper assumes packed readback at bits 2/3. Claim only a shared grant, leaving
       exclusive grants alone. Keep both the SDK mutex and the critical section for unlocked Wi-Fi
       users. */
    const uint32_t grants = SEC_ENG_SE_AES_ID0_EN | SEC_ENG_SE_AES_ID1_EN;
    if (((getreg32(dev->reg_base + SEC_ENG_SE_CTRL_PROT_RD_OFFSET) >> 4) & grants) == grants)
        putreg32(SEC_ENG_SE_AES_ID0_EN, dev->reg_base + SEC_ENG_SE_AES_0_CTRL_PROT_OFFSET);
    if (((getreg32(dev->reg_base + SEC_ENG_SE_CTRL_PROT_RD_OFFSET) >> 4) & grants) !=
        SEC_ENG_SE_AES_ID0_EN)
        aes_fail("engine grant mismatch", dev);
#else
    unsigned owner = 1;
#if defined(BL618DG)
    if (GLB_Get_Core_Type() == GLB_CORE_ID_NP) {
        bflb_group1_request_aes_access(dev);
        owner = 2;
    } else
#endif
    {
        bflb_group0_request_aes_access(dev);
    }
    /* The SDK request also returns failure when our group already owns the engine.
       Check the effective grant, never accept an engine owned by the other core. */
    if (((getreg32(dev->reg_base + SEC_ENG_SE_CTRL_PROT_RD_OFFSET) >> 2) & 3) != owner) {
        aes_fail("engine grant mismatch", dev);
    }
#endif
    uint64_t start = bflb_mtimer_get_time_ms();
    while (getreg32(dev->reg_base + SEC_ENG_SE_AES_0_CTRL_OFFSET) & SEC_ENG_SE_AES_0_BUSY) {
        /* Re-read after the deadline: preemption alone must not look like a stall. */
        if (bflb_mtimer_get_time_ms() - start > 100 &&
            (getreg32(dev->reg_base + SEC_ENG_SE_AES_0_CTRL_OFFSET) & SEC_ENG_SE_AES_0_BUSY))
            aes_fail("timeout before initialization", dev);
    }
    memset(&descriptor, 0, sizeof descriptor);
    descriptor.aes_mode = AES_MODE_ECB;
    descriptor.aes_key = key_len == 16   ? AES_LINK_KEY_128BITS
                         : key_len == 24 ? AES_LINK_KEY_192BITS
                                         : AES_LINK_KEY_256BITS;
    descriptor.aes_dec_en = decrypt != 0;
    memcpy(&descriptor.aes_key0, key, key_len);
    memcpy(source, input, num * 16);
    bflb_aes_link_init(dev);
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    int ret = bflb_aes_link_update(dev, (uint32_t)(uintptr_t)&descriptor, source, result, num * 16);
    if (ret != 0)
        aes_fail(ret == -ETIMEDOUT ? "timeout after trigger" : "link update failed", dev);
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    memcpy(output, result, num * 16);
    bflb_aes_link_deinit(dev);
    nm_crypto_zero(&descriptor, sizeof descriptor);
    nm_crypto_zero(source, num * 16);
    nm_crypto_zero(result, num * 16);
    taskEXIT_CRITICAL();
}

void newmota_aes_blocks(const void* key, size_t key_len, int decrypt, const unsigned char* input,
    unsigned char* output, size_t num) {
    if (key_len != 16 && key_len != 24 && key_len != 32)
        aes_fail("invalid key length", NULL);
    struct bflb_device_s* dev = bflb_device_get_by_name(BFLB_NAME_SEC_AES);
    if (!dev)
        aes_fail("device unavailable", NULL);
    if (bflb_sec_aes_mutex_take() != 0)
        aes_fail("mutex take failed", NULL);
    while (num) {
        size_t run = num < NEWMOTA_AES_RUN_BLOCKS ? num : NEWMOTA_AES_RUN_BLOCKS;
        run_chunk(dev, key, key_len, decrypt, input, output, run);
        input += run * 16;
        output += run * 16;
        num -= run;
    }
    if (bflb_sec_aes_mutex_give() != 0)
        aes_fail("mutex give failed", NULL);
}
