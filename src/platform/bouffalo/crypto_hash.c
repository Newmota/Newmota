/*
  crypto_hash.c - Bouffalo resumable hardware SHA for BearSSL

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file
 * @brief Compress block runs using DMA link descriptors carrying arbitrary state.
 * Buffers are shared only while holding the SDK SHA mutex. BL602 descriptors live
 * in Wi-Fi SRAM because its engine cannot reliably update descriptors in DTCM.
 * The pinned SDK mbedtls_sha*_update_ret paths copy each saved link descriptor
 * into shared DMA storage under the same mutex, establishing descriptor-based
 * state restoration. bflb_sha*_link_finish copies result bytes directly into the
 * digest, so the descriptor stores big-endian digest bytes.
 * A timeout aborts while retaining ownership and static DMA storage: no active
 * engine can access a returned stack frame or race a subsequent operation.
 */
#include "newmota/platform/crypto_hash.h"
#include "newmota_features.h"
#include "inner.h"
#include "newmota_crypto.h"
#include "bflb_sec_sha.h"
#include "bflb_sec_mutex.h"
#include "hardware/sec_eng_reg.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(BL618DG)
#include "bl618dg_glb.h"
#endif

/* One trigger hashes a chunk of RUN_BYTES; .wifi_ram leftovers feed the BL602 Wi-Fi heap. */
#if defined(BL602)
#define DMA_STORAGE __attribute__((section(".wifi_ram"), aligned(32)))
#define RUN_BYTES 256
#else
#define DMA_STORAGE ATTR_NOCACHE_NOINIT_RAM_SECTION __attribute__((aligned(32)))
#define RUN_BYTES 1024
#endif
#if USE_CRYPTO_HW_SHA512 && defined(BL602)
#error "BL602 has no SHA-512 engine"
#endif
static DMA_STORAGE struct bflb_sha_link_s descriptor;
static DMA_STORAGE unsigned char message[RUN_BYTES];
/* Device table entries are static; racing first lookups store the same pointer. */
static struct bflb_device_s* sha_dev;

/** @brief Report failure without releasing engine ownership or touching hash data.
 * Read descriptor metadata only after this run initialized and triggered it. */
static _Noreturn void sha_fail(const char* reason, struct bflb_device_s* dev, bool triggered) {
    printf("Bouffalo SHA failure: %s\r\n", reason);
    if (dev) {
        printf("SHA ctrl=%08x status=%08x link=%08x prot=%08x\r\n",
            (unsigned)getreg32(dev->reg_base + SEC_ENG_SE_SHA_0_CTRL_OFFSET),
            (unsigned)getreg32(dev->reg_base + SEC_ENG_SE_SHA_0_STATUS_OFFSET),
            (unsigned)getreg32(dev->reg_base + SEC_ENG_SE_SHA_0_LINK_OFFSET),
            (unsigned)getreg32(dev->reg_base + SEC_ENG_SE_CTRL_PROT_RD_OFFSET));
    }
    if (triggered) {
        uint32_t descriptor_ctrl;
        memcpy(&descriptor_ctrl, &descriptor, sizeof descriptor_ctrl);
        printf("SHA descriptor=%p ctrl=%08x src=%08x\r\n", (void*)&descriptor,
            (unsigned)descriptor_ctrl, (unsigned)descriptor.sha_srcaddr);
    }
    abort();
}

/** @brief Wait for idle; a confirmed timeout keeps the mutex and DMA storage.
 * An idle engine returns before the timer is read. */
static void wait_idle(struct bflb_device_s* dev, bool triggered) {
    if (!(getreg32(dev->reg_base + SEC_ENG_SE_SHA_0_CTRL_OFFSET) & SEC_ENG_SE_SHA_0_BUSY))
        return;
    uint64_t start = bflb_mtimer_get_time_ms();
    while (getreg32(dev->reg_base + SEC_ENG_SE_SHA_0_CTRL_OFFSET) & SEC_ENG_SE_SHA_0_BUSY) {
        /* Re-read after the deadline: preemption alone must not look like a stall. */
        if (bflb_mtimer_get_time_ms() - start > 100 &&
            (getreg32(dev->reg_base + SEC_ENG_SE_SHA_0_CTRL_OFFSET) & SEC_ENG_SE_SHA_0_BUSY))
            sha_fail(triggered ? "timeout after trigger" : "timeout before initialization", dev,
                triggered);
    }
}

/** @brief Restore a serialized SHA midstate and compress a run of complete blocks.
 * The mutex, engine grant, link mode and midstate load are taken once per run;
 * each chunk of up to RUN_BYTES is one trigger, and the engine accumulates the
 * midstate in the descriptor between chunks. */
static void compress(
    unsigned mode, const unsigned char* buf, size_t num, void* state, size_t size) {
    const size_t block_len = mode == SHA_MODE_SHA512 ? 128 : 64;
    struct bflb_device_s* dev = sha_dev;
    if (!dev) {
        dev = bflb_device_get_by_name(BFLB_NAME_SEC_SHA);
        if (!dev)
            sha_fail("device unavailable", NULL, false);
        sha_dev = dev;
    }
    if (bflb_sec_sha_mutex_take() != 0)
        sha_fail("mutex take failed", NULL, false);
#if defined(BL602)
    /* BL602 has SHA PROT_EN at bit 0 and ID0/ID1 enables at bits 1/2 in both
       the readback (0xF00) and the writable register (bl_iot_sdk@14ebffe9,
       BL602 sec_eng_reg.h; not in the pinned SDK). The SDK request
       helper assumes the newer packed readback layout, so claim only a shared
       grant here and leave an existing exclusive grant alone. Keep the SDK
       mutex: Wi-Fi/mbedTLS callers must serialize access to the same engine. */
    const uint32_t grants = SEC_ENG_SE_SHA_ID0_EN | SEC_ENG_SE_SHA_ID1_EN;
    if ((getreg32(dev->reg_base + SEC_ENG_SE_CTRL_PROT_RD_OFFSET) & grants) == grants)
        putreg32(SEC_ENG_SE_SHA_ID0_EN, dev->reg_base + SEC_ENG_SE_SHA_0_CTRL_PROT_OFFSET);
    if ((getreg32(dev->reg_base + SEC_ENG_SE_CTRL_PROT_RD_OFFSET) & grants) !=
        SEC_ENG_SE_SHA_ID0_EN)
        sha_fail("engine grant mismatch", dev, false);
#else
    unsigned owner = 1;
#if defined(BL618DG)
    if (GLB_Get_Core_Type() == GLB_CORE_ID_NP) {
        bflb_group1_request_sha_access(dev);
        owner = 2;
    } else
#endif
    {
        bflb_group0_request_sha_access(dev);
    }
    /* The SDK request also returns failure when our group already owns the engine.
       Check the effective grant, never accept an engine owned by the other core. */
    if (((getreg32(dev->reg_base + SEC_ENG_SE_CTRL_PROT_RD_OFFSET) >> 0) & 3) != owner) {
        sha_fail("engine grant mismatch", dev, false);
    }
#endif
    wait_idle(dev, false);
    bflb_sha_link_init(dev);
    /* The control word and source address are cleared together: storage is noinit. */
    memset(&descriptor, 0, offsetof(struct bflb_sha_link_s, result));
    memcpy(descriptor.result, state, size);
    descriptor.sha_mode = mode;
    descriptor.sha_newhash_dis = 1;
    descriptor.sha_srcaddr = (uint32_t)(uintptr_t)message;
    putreg32((uint32_t)(uintptr_t)&descriptor, dev->reg_base + SEC_ENG_SE_SHA_0_LINK_OFFSET);
    const size_t run_blocks = RUN_BYTES / block_len;
    size_t used = 0;
    while (num) {
        const size_t blocks = num < run_blocks ? num : run_blocks;
        const size_t len = blocks * block_len;
        memcpy(message, buf, len);
        if (len > used)
            used = len;
        descriptor.sha_msglen = blocks;
        __asm__ volatile("fence iorw, iorw" ::: "memory");
        uint32_t ctrl = getreg32(dev->reg_base + SEC_ENG_SE_SHA_0_CTRL_OFFSET);
        putreg32(ctrl | SEC_ENG_SE_SHA_0_TRIG_1T, dev->reg_base + SEC_ENG_SE_SHA_0_CTRL_OFFSET);
        wait_idle(dev, true);
        __asm__ volatile("fence iorw, iorw" ::: "memory");
        buf += len;
        num -= blocks;
    }
    memcpy(state, descriptor.result, size);
    /* Leave the engine out of link mode, as the SDK's non-link users expect. */
    bflb_sha_link_deinit(dev);
    nm_crypto_zero(message, used);
    nm_crypto_zero(descriptor.result, size);
    if (bflb_sec_sha_mutex_give() != 0)
        sha_fail("mutex give failed", NULL, false);
}
#if USE_CRYPTO_HW_SHA1
void newmota_sha1_blocks(const unsigned char* buf, size_t num, uint32_t* val) {
    uint32_t state[5];
    br_range_enc32be(state, val, 5);
    compress(SHA_MODE_SHA1, buf, num, state, sizeof state);
    br_range_dec32be(val, 5, state);
    nm_crypto_zero(state, sizeof state);
}
#endif
#if USE_CRYPTO_HW_SHA256
void newmota_sha256_blocks(const unsigned char* buf, size_t num, uint32_t* val) {
    uint32_t state[8];
    br_range_enc32be(state, val, 8);
    compress(SHA_MODE_SHA256, buf, num, state, sizeof state);
    br_range_dec32be(val, 8, state);
    nm_crypto_zero(state, sizeof state);
}
#endif
#if USE_CRYPTO_HW_SHA512
void newmota_sha512_blocks(const unsigned char* buf, size_t num, uint64_t* val) {
    uint64_t state[8];
    br_range_enc64be(state, val, 8);
    compress(SHA_MODE_SHA512, buf, num, state, sizeof state);
    br_range_dec64be(val, 8, state);
    nm_crypto_zero(state, sizeof state);
}
#endif
