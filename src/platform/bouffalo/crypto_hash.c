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
#include "newmota_features.h"
#include "newmota/platform/crypto_hash.h"
#include "newmota/platform/bearssl_inner.h"
#include "newmota_crypto.h"
#include "bflb_sec_sha.h"
#include "bflb_sec_mutex.h"
#include "bflb_mtimer.h"
#include "hardware/sec_eng_reg.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/** @brief Busy polls before the timer-checked wait. One block takes a few hundred
 * cycles; bflb_mtimer_get_time_ms() divides 64-bit microseconds on every call. */
#define SPIN_POLLS 1024

/** @brief Wait for idle; a confirmed timeout keeps the mutex and DMA storage. */
static void wait_idle(struct bflb_device_s* dev, bool triggered) {
    for (unsigned i = 0; i < SPIN_POLLS; ++i) {
        if (!(getreg32(dev->reg_base + SEC_ENG_SE_SHA_0_CTRL_OFFSET) & SEC_ENG_SE_SHA_0_BUSY))
            return;
    }
    uint64_t start = bflb_mtimer_get_time_ms();
    while (getreg32(dev->reg_base + SEC_ENG_SE_SHA_0_CTRL_OFFSET) & SEC_ENG_SE_SHA_0_BUSY) {
        /* Re-read after the deadline: preemption alone must not look like a stall. */
        if (bflb_mtimer_get_time_ms() - start > 100 &&
            (getreg32(dev->reg_base + SEC_ENG_SE_SHA_0_CTRL_OFFSET) & SEC_ENG_SE_SHA_0_BUSY))
            sha_fail(triggered ? "timeout after trigger" : "timeout before initialization", dev,
                triggered);
    }
}

/** @brief Clear DMA storage with word stores. Byte-wise memset costs about 12 cycles
 * per byte in BL616 non-cached RAM; @p p is word aligned and @p n a multiple of 4. */
static void dma_zero(void* p, size_t n) {
    volatile uint32_t* w = p;
    for (size_t i = 0; i < n / 4; ++i)
        w[i] = 0;
}

/** @brief Take the mutex and grant, enter link mode and point the engine at the
 * descriptor; the caller loads descriptor.result and message. */
static struct bflb_device_s* engine_take(unsigned mode) {
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
    descriptor.sha_mode = mode;
    descriptor.sha_newhash_dis = 1;
    descriptor.sha_srcaddr = (uint32_t)(uintptr_t)message;
    putreg32((uint32_t)(uintptr_t)&descriptor, dev->reg_base + SEC_ENG_SE_SHA_0_LINK_OFFSET);
    return dev;
}

/** @brief Hash @p blocks blocks of message onto descriptor.result. */
static void engine_run(struct bflb_device_s* dev, size_t blocks) {
    descriptor.sha_msglen = blocks;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    uint32_t ctrl = getreg32(dev->reg_base + SEC_ENG_SE_SHA_0_CTRL_OFFSET);
    putreg32(ctrl | SEC_ENG_SE_SHA_0_TRIG_1T, dev->reg_base + SEC_ENG_SE_SHA_0_CTRL_OFFSET);
    wait_idle(dev, true);
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}

/** @brief Leave link mode, clear the used message bytes and the state, release the mutex. */
static void engine_give(struct bflb_device_s* dev, size_t used, size_t size) {
    /* Leave the engine out of link mode, as the SDK's non-link users expect. */
    bflb_sha_link_deinit(dev);
    dma_zero(message, used);
    dma_zero(descriptor.result, size);
    if (bflb_sec_sha_mutex_give() != 0)
        sha_fail("mutex give failed", NULL, false);
}

/** @brief Restore a serialized SHA midstate and compress a run of complete blocks.
 * The mutex, engine grant, link mode and midstate load are taken once per run;
 * each chunk of up to RUN_BYTES is one trigger, and the engine accumulates the
 * midstate in the descriptor between chunks. */
static void compress(
    unsigned mode, const unsigned char* buf, size_t num, void* state, size_t size) {
    const size_t block_len = mode == SHA_MODE_SHA512 ? 128 : 64;
    struct bflb_device_s* dev = engine_take(mode);
    memcpy(descriptor.result, state, size);
    const size_t run_blocks = RUN_BYTES / block_len;
    size_t used = 0;
    while (num) {
        const size_t blocks = num < run_blocks ? num : run_blocks;
        const size_t len = blocks * block_len;
        memcpy(message, buf, len);
        if (len > used)
            used = len;
        engine_run(dev, blocks);
        buf += len;
        num -= blocks;
    }
    memcpy(state, descriptor.result, size);
    engine_give(dev, used, size);
}

/** @brief Native words to and from the engine's big-endian byte words, as shifts:
 * without Zbb, GCC turns __builtin_bswap32 into a __bswapsi2 call per word; the shift
 * form stays inline at -Os on RISC-V (unlike Xtensa, see esp-idf/crypto_hash.c). */
static void swap32(uint32_t* dst, const uint32_t* src, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const uint32_t t = (src[i] << 16) | (src[i] >> 16);
        dst[i] = ((t & 0x00ff00ffu) << 8) | ((t >> 8) & 0x00ff00ffu);
    }
}

#if USE_CRYPTO_HW_SHA1
void newmota_sha1_blocks(const unsigned char* buf, size_t num, uint32_t* val) {
    uint32_t state[5];
    swap32(state, val, 5);
    compress(SHA_MODE_SHA1, buf, num, state, sizeof state);
    swap32(val, state, 5);
    nm_crypto_zero(state, sizeof state);
}
#endif
#if USE_CRYPTO_HW_SHA256
void newmota_sha256_blocks(const unsigned char* buf, size_t num, uint32_t* val) {
    uint32_t state[8];
    swap32(state, val, 8);
    compress(SHA_MODE_SHA256, buf, num, state, sizeof state);
    swap32(val, state, 8);
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

#if USE_CRYPTO_HW_SHA1 && USE_WIFI_WPA3_BEARSSL
/*
 * PBKDF2-HMAC-SHA1 for the supplicant's PSK, as on ESP (esp-idf/crypto_hash.c). BL616 and
 * BL618DG call it from hostap_hash_bearssl.c's pbkdf2_sha1(); on BL602 the link wraps the
 * SDK's pbkdf2_sha1 (native/CMakeLists.txt), whose hostap loop rehashes both key pads with
 * four engine runs per iteration. Here the pads are hashed once into midstates and each
 * HMAC is one block from the inner and one from the outer midstate, with the engine held
 * across PBKDF2_LOCK_ITERATIONS iterations. All words stay in engine byte order: a digest
 * read from the descriptor is already the next message's bytes. Within an iteration only
 * the 20 digest bytes are copied: the message's padding for 64 + 20 bytes stays in place.
 */
#define PBKDF2_LOCK_ITERATIONS 64
#define PBKDF2_MAX_SSID 51 /* SSID, count, 0x80 and the length still fit one block */

/** @brief One SHA-1 block continued from @p mid; the first @p len message bytes are
 * replaced by @p head. Engine held in SHA-1 link mode. */
static void sha1_block_from(struct bflb_device_s* dev, const uint32_t* mid, const void* head,
    size_t len, uint32_t* out) {
    memcpy(descriptor.result, mid, 20);
    memcpy(message, head, len);
    engine_run(dev, 1);
    memcpy(out, descriptor.result, 20);
}

/** @brief Big-endian message bit count in the last eight bytes of a final block. */
static void put_bit_length(uint32_t* block, size_t bytes) {
    unsigned char* b = (unsigned char*)block;
    const uint64_t bits = (uint64_t)bytes * 8;
    for (int i = 0; i < 8; ++i)
        b[63 - i] = (unsigned char)(bits >> (8 * i));
}

int newmota_hw_pbkdf2_sha1(const char* passphrase, const unsigned char* ssid, size_t ssid_len,
    int iterations, unsigned char* buf, size_t buflen) {
    if (iterations < 1 || ssid_len > PBKDF2_MAX_SSID)
        return 1;
    static const uint32_t sha1_iv[5] = {
        0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    uint32_t key[16] = {0}, iv[5], ipad[5], opad[5], block[16], u[5], t[5];
    size_t key_len = strlen(passphrase);
    if (key_len > sizeof key) {
        /* Before engine_take(): br_sha1 takes the engine itself. */
        br_sha1_context ctx;
        br_sha1_init(&ctx);
        br_sha1_update(&ctx, passphrase, key_len);
        br_sha1_out(&ctx, key);
        nm_crypto_zero(&ctx, sizeof ctx);
    } else {
        memcpy(key, passphrase, key_len);
    }
    swap32(iv, sha1_iv, 5);

    struct bflb_device_s* dev = engine_take(SHA_MODE_SHA1);
    for (size_t i = 0; i < 16; ++i)
        block[i] = key[i] ^ 0x36363636u;
    sha1_block_from(dev, iv, block, sizeof block, ipad);
    for (size_t i = 0; i < 16; ++i)
        block[i] = key[i] ^ 0x5c5c5c5cu;
    sha1_block_from(dev, iv, block, sizeof block, opad);

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
        sha1_block_from(dev, ipad, block, sizeof block, u);
        /* Every later block is a 20-byte message after a 64-byte pad. */
        memset(block, 0, sizeof block);
        b[20] = 0x80;
        put_bit_length(block, 64 + 20);
        memcpy(block, u, 20);
        sha1_block_from(dev, opad, block, sizeof block, u);
        memcpy(t, u, sizeof t);
        for (int j = 1; j < iterations; ++j) {
            if (j % PBKDF2_LOCK_ITERATIONS == 0) {
                /* Let other engine users in; engine_give() clears the message, so
                   restore the padding after the 20 digest bytes each run replaces. */
                engine_give(dev, sizeof block, 20);
                dev = engine_take(SHA_MODE_SHA1);
                memcpy(message + 20, (const unsigned char*)block + 20, sizeof block - 20);
            }
            sha1_block_from(dev, ipad, u, 20, u);
            sha1_block_from(dev, opad, u, 20, u);
            for (size_t k = 0; k < 5; ++k)
                t[k] ^= u[k];
        }
        const size_t n = left < sizeof t ? left : sizeof t;
        memcpy(pos, t, n);
        pos += n;
        left -= n;
    }
    engine_give(dev, sizeof block, 20);
    nm_crypto_zero(key, sizeof key);
    nm_crypto_zero(ipad, sizeof ipad);
    nm_crypto_zero(opad, sizeof opad);
    nm_crypto_zero(block, sizeof block);
    nm_crypto_zero(u, sizeof u);
    nm_crypto_zero(t, sizeof t);
    return 0;
}

#if defined(BL602)
int __real_pbkdf2_sha1(const char* passphrase, const char* ssid, size_t ssid_len, int iterations,
    uint8_t* buf, size_t buflen);
int __wrap_pbkdf2_sha1(const char* passphrase, const char* ssid, size_t ssid_len, int iterations,
    uint8_t* buf, size_t buflen);
int __wrap_pbkdf2_sha1(const char* passphrase, const char* ssid, size_t ssid_len, int iterations,
    uint8_t* buf, size_t buflen) {
    if (newmota_hw_pbkdf2_sha1(
            passphrase, (const unsigned char*)ssid, ssid_len, iterations, buf, buflen) == 0)
        return 0;
    return __real_pbkdf2_sha1(passphrase, ssid, ssid_len, iterations, buf, buflen);
}
#endif
#endif
