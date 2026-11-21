/*
  crypto_module.cpp - Tasmota-compatible Berry crypto backed by BearSSL

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/**
 * @file crypto_module.cpp
 * @brief Tasmota-compatible Berry crypto contracts backed by portable BearSSL.
 *
 * Bytes inputs are checked before use; wrong types raise type_error, invalid
 * lengths/parameters/state raise value_error, and allocation failures raise
 * memory_error. Unless documented below, inputs are preserved and returned
 * bytes own independent storage. Contexts are GC owned and cleared on release.
 */
/* API reference: Tasmota 48da93dc xdrv_52_3_berry_crypto.ino and be_crypto_lib.c. */
#include "newmota_features.h"
extern "C" {
#include "berry.h"
#include "be_constobj.h"
#include "be_object.h"
#include "be_gc.h"
#include "be_byteslib.h"
#include "bearssl.h"
#include "bearssl_newmota_config.h"
void br_i15_decode(uint16_t*, const void*, size_t);
void br_i15_decode_reduce(uint16_t*, const void*, size_t, const uint16_t*);
void br_i15_encode(void*, size_t, const uint16_t*);
uint32_t br_i15_sub(uint16_t*, const uint16_t*, uint32_t);
#include "newmota_crypto.h"
}
#include "newmota/platform/crypto_random.h"
#include <cstdlib>
#include <cstring>
#include <cstdint>

#if USE_BERRY_CRYPTO_SPAKE2P_MATTER
extern "C" const bclass be_class_SPAKE2P_Matter;
#endif

namespace {
/** @brief Borrowed bytes view; its backing Berry value must remain rooted during use. */
struct Bytes {
    unsigned char* data;
    size_t size;
};

/** @brief Borrow a checked bytes argument; optionally enforce exact length.
 * Raises type_error for other types and value_error for a length mismatch. */
[[maybe_unused]] Bytes bytes(bvm* vm, int index, size_t expected = SIZE_MAX) {
    if (index > be_top(vm) || !be_isbytes(vm, index))
        be_raise(vm, "type_error", "expected bytes");
    size_t n;
    auto* p = static_cast<unsigned char*>(const_cast<void*>(be_tobytes(vm, index, &n)));
    if (expected != SIZE_MAX && n != expected)
        be_raise(vm, "value_error", "invalid byte length");
    return {p, n};
}

/** @brief Borrow mutable bytes, rejecting solidified readonly storage before writes. */
[[maybe_unused]] Bytes writable_bytes(bvm* vm, int index, size_t expected = SIZE_MAX) {
    auto data = bytes(vm, index, expected);
    be_getmember(vm, index, ".size");
    const bool readonly = be_toint(vm, -1) == BYTES_SIZE_SOLIDIFIED;
    be_pop(vm, 1);
    if (readonly)
        be_raise(vm, "value_error", BYTES_READ_ONLY_MESSAGE);
    return data;
}

/** @brief Check overlap of nonempty borrowed ranges without pointer-end overflow. */
[[maybe_unused]] bool overlap(const Bytes& a, const Bytes& b) {
    if (!a.size || !b.size)
        return false;
    auto x = reinterpret_cast<uintptr_t>(a.data), y = reinterpret_cast<uintptr_t>(b.data);
    return x <= y ? y - x < a.size : x - y < b.size;
}

/** @brief Allocate and root an independent Berry bytes result of exactly n bytes.
 * Raise memory_error when the VM allocation cap prevents an exact allocation. */
[[maybe_unused]] Bytes output(bvm* vm, size_t n) {
    be_pushbytes(vm, nullptr, n);
    size_t actual;
    auto* p = static_cast<unsigned char*>(const_cast<void*>(be_tobytes(vm, -1, &actual)));
    if (actual != n)
        be_raise(vm, "memory_error", "crypto output allocation failed");
    return {p, n};
}

/** @brief Raise value_error with the supplied contract failure message. */
[[maybe_unused]] void require(bvm* vm, bool ok, const char* why) {
    if (!ok)
        be_raise(vm, "value_error", why);
}

/** @brief Read an integer argument, raising type_error for missing/noninteger values. */
[[maybe_unused]] bint integer(bvm* vm, int index) {
    if (index > be_top(vm) || !be_isint(vm, index))
        be_raise(vm, "type_error", "expected integer");
    return be_toint(vm, index);
}

/** @brief GC-owned incremental algorithm state and AEAD lifecycle counters.
 * kind validates dispatch; finalized prevents updates after tag/finish, mode_set
 * prevents AEAD direction changes, and expected/processed bound CCM payloads.
 * The owning common object clears this allocation when released. */
struct Context {
    enum Kind { md5, sha256, hmac, ctr, gcm, ccm, blake2s } kind;

    size_t expected, processed, tag_len;
    bool finalized;
    bool mode_set;
    bool encrypt;
    unsigned char tag[16];
    br_md5_context md;
    br_sha256_context sha;
    br_hmac_context mac;
    br_aes_small_ctr_keys aes_ctr;
    br_aes_small_ctrcbc_keys aes_ccm;
    br_gcm_context gcm_ctx;
    br_ccm_context ccm_ctx;
    nm_blake2s_context blake;
};

[[maybe_unused]] int destroy_context(bvm* vm) {
    auto* ctx = static_cast<Context*>(be_tocomptr(vm, 1));
    if (ctx) {
        nm_crypto_zero(ctx, sizeof *ctx);
        std::free(ctx);
    }
    be_return_nil(vm);
}

[[maybe_unused]] Context* allocate_context(bvm* vm, Context::Kind kind) {
    // Root the common object before allocating its payload. Allocation failure
    // cannot strand a context outside the GC ownership graph.
    be_newcomobj(vm, nullptr, destroy_context);
    auto* ctx = static_cast<Context*>(std::calloc(1, sizeof(Context)));
    if (!ctx)
        be_raise(vm, "memory_error", "crypto context allocation failed");
    static_cast<bcommomobj*>(var_toobj(be_indexof(vm, -1)))->data = ctx;
    ctx->kind = kind;
    be_setmember(vm, 1, ".p");
    be_pop(vm, 1);
    return ctx;
}

[[maybe_unused]] Context* context(bvm* vm, Context::Kind kind) {
    be_getmember(vm, 1, ".p");
    auto* ctx = static_cast<Context*>(be_tocomptr(vm, -1));
    be_pop(vm, 1);
    if (!ctx || ctx->kind != kind)
        be_raise(vm, "value_error", "invalid crypto context");
    return ctx;
}
#if USE_BERRY_CRYPTO_MD5
/** @brief MD5(): create an empty incremental MD5 context. */
int md5_init(bvm* vm) {
    br_md5_init(&allocate_context(vm, Context::md5)->md);
    be_return_nil(vm);
}

/** @brief MD5.update(data: bytes) -> self; append data, rejecting updates after finish(). */
int md5_update(bvm* vm) {
    auto data = bytes(vm, 2);
    auto* ctx = context(vm, Context::md5);
    require(vm, !ctx->finalized, "hash already finalized");
    br_md5_update(&ctx->md, data.data, data.size);
    be_pushvalue(vm, 1);
    be_return(vm);
}

/** @brief MD5.finish() -> bytes(16); finalize updates, with repeatable digest reads. */
int md5_finish(bvm* vm) {
    auto* ctx = context(vm, Context::md5);
    auto out = output(vm, 16);
    br_md5_out(&ctx->md, out.data);
    ctx->finalized = true;
    be_return(vm);
}
#endif
#if USE_BERRY_CRYPTO_SHA256
/** @brief SHA256(): create an empty incremental SHA-256 context. */
int sha_init(bvm* vm) {
    br_sha256_init(&allocate_context(vm, Context::sha256)->sha);
    be_return_nil(vm);
}

/** @brief SHA256.update(data: bytes) -> self; append arbitrary bytes. */
int sha_update(bvm* vm) {
    auto data = bytes(vm, 2);
    auto* ctx = context(vm, Context::sha256);
    br_sha256_update(&ctx->sha, data.data, data.size);
    be_pushvalue(vm, 1);
    be_return(vm);
}

/** @brief SHA256.out() -> bytes(32); snapshot digest without finalizing updates. */
int sha_out(bvm* vm) {
    auto* ctx = context(vm, Context::sha256);
    auto out = output(vm, 32);
    br_sha256_out(&ctx->sha, out.data);
    be_return(vm);
}
#endif
#if USE_BERRY_CRYPTO_HMAC_SHA256
/** @brief HMAC_SHA256(key: bytes): initialize keyed incremental SHA-256. */
int hmac_init(bvm* vm) {
    auto key = bytes(vm, 2);
    auto* ctx = allocate_context(vm, Context::hmac);
    br_hmac_key_context k;
    br_hmac_key_init(&k, &br_sha256_vtable, key.data, key.size);
    br_hmac_init(&ctx->mac, &k, 0);
    nm_crypto_zero(&k, sizeof k);
    be_return_nil(vm);
}

/** @brief HMAC_SHA256.update(data: bytes) -> self; append arbitrary bytes. */
int hmac_update(bvm* vm) {
    auto data = bytes(vm, 2);
    auto* ctx = context(vm, Context::hmac);
    br_hmac_update(&ctx->mac, data.data, data.size);
    be_pushvalue(vm, 1);
    be_return(vm);
}

/** @brief HMAC_SHA256.out() -> bytes(32); repeatable snapshot, allowing further updates. */
int hmac_out(bvm* vm) {
    auto* ctx = context(vm, Context::hmac);
    auto out = output(vm, 32);
    br_hmac_out(&ctx->mac, out.data);
    be_return(vm);
}
#endif
#if USE_BERRY_CRYPTO_HKDF_SHA256
/** @brief HKDF_SHA256.derive(ikm, salt, info: bytes, out_bytes: int) -> bytes.
 * Output length is 1..256; empty salt/info are permitted. */
int hkdf_derive(bvm* vm) {
    auto ikm = bytes(vm, 1), salt = bytes(vm, 2), info = bytes(vm, 3);
    auto n = integer(vm, 4);
    require(vm, n >= 1 && n <= 256, "invalid out_bytes");
    auto out = output(vm, static_cast<size_t>(n));
    br_hkdf_context ctx;
    br_hkdf_init(&ctx, &br_sha256_vtable, salt.data, salt.size);
    br_hkdf_inject(&ctx, ikm.data, ikm.size);
    br_hkdf_flip(&ctx);
    auto result = br_hkdf_produce(&ctx, info.data, info.size, out.data, out.size);
    nm_crypto_zero(&ctx, sizeof ctx);
    require(vm, result == out.size, "HKDF failed");
    be_return(vm);
}
#endif
#if USE_BERRY_CRYPTO_PBKDF2_HMAC_SHA256
Bytes password(bvm* vm, int index) {
    if (be_isstring(vm, index))
        return {reinterpret_cast<unsigned char*>(const_cast<char*>(be_tostring(vm, index))),
            static_cast<size_t>(be_strlen(vm, index))};
    return bytes(vm, index);
}

/** @brief PBKDF2_HMAC_SHA256().derive(password, salt, iterations, length) -> bytes.
 * Nonempty password/salt accept string or bytes; iterations 1..10000, length 1..8192. */
int pbkdf2_derive(bvm* vm) {
    auto p = password(vm, 2), s = password(vm, 3);
    auto count = integer(vm, 4), n = integer(vm, 5);
    require(vm, p.size && s.size, "password and salt must not be empty");
    require(vm, count >= 1 && count <= 10000, "invalid iterations number");
    require(vm, n >= 1 && n <= 8192, "derived key length must be 1..8192");
    auto out = output(vm, static_cast<size_t>(n));
    require(vm,
        nm_pbkdf2_sha256(
            p.data, p.size, s.data, s.size, static_cast<uint32_t>(count), out.data, out.size),
        "PBKDF2 failed");
    be_return(vm);
}

/** @brief PBKDF2_HMAC_SHA256._f(password, salt_i: bytes, iterations: int, result: bytes(32)) ->
 * nil. Nonempty inputs, iterations 1..10000; writes the complete block to writable result. */
int pbkdf2_f(bvm* vm) {
    auto p = bytes(vm, 1), s = bytes(vm, 2);
    auto count = integer(vm, 3);
    auto result = writable_bytes(vm, 4, 32);
    require(vm, p.size && s.size, "password and salt must not be empty");
    require(vm, count >= 1 && count <= 10000, "invalid iterations number");
    br_hmac_key_context key;
    br_hmac_context mac;
    unsigned char u[32], acc[32];
    br_hmac_key_init(&key, &br_sha256_vtable, p.data, p.size);
    br_hmac_init(&mac, &key, 0);
    br_hmac_update(&mac, s.data, s.size);
    br_hmac_out(&mac, u);
    std::memcpy(acc, u, 32);
    for (bint i = 1; i < count; ++i) {
        br_hmac_init(&mac, &key, 0);
        br_hmac_update(&mac, u, 32);
        br_hmac_out(&mac, u);
        for (unsigned j = 0; j < 32; ++j)
            acc[j] ^= u[j];
    }
    std::memcpy(result.data, acc, 32);
    nm_crypto_zero(u, sizeof u);
    nm_crypto_zero(acc, sizeof acc);
    nm_crypto_zero(&key, sizeof key);
    nm_crypto_zero(&mac, sizeof mac);
    be_return_nil(vm);
}
#endif
#if USE_BERRY_CRYPTO_ED25519
/** @brief ED25519().secret_key(seed: bytes(32)) -> bytes(64), encoded seed || public key. */
int ed_secret(bvm* vm) {
    auto seed = bytes(vm, 2, 32);
    auto out = output(vm, 64);
    require(vm, nm_ed25519_secret_key(seed.data, out.data), "invalid seed");
    be_return(vm);
}

/** @brief ED25519().sign(message: bytes, secret_key: bytes(64)) -> bytes(64).
 * Reject inconsistent seed/public-key pairs; preserves both inputs. */
int ed_sign(bvm* vm) {
    auto message = bytes(vm, 2), key = bytes(vm, 3, 64);
    auto out = output(vm, 64);
    require(
        vm, nm_ed25519_sign(key.data, message.data, message.size, out.data), "invalid secret key");
    be_return(vm);
}

/** @brief ED25519().verify(message: bytes, signature: bytes(64), public_key: bytes(32)) -> bool.
 * Invalid signatures return false; malformed lengths raise value_error. */
int ed_verify(bvm* vm) {
    auto message = bytes(vm, 2), signature = bytes(vm, 3, 64), key = bytes(vm, 4, 32);
    be_pushbool(vm, nm_ed25519_verify(key.data, message.data, message.size, signature.data));
    be_return(vm);
}
#endif
#if USE_BERRY_CRYPTO_EC_C25519
void c25519_scalar(unsigned char reversed[32], const Bytes& key) {
    for (unsigned i = 0; i < 32; ++i)
        reversed[i] = key.data[31 - i];
}

/** @brief EC_C25519().public_key(private_key: bytes(32)) -> bytes(32).
 * Keys use RFC 7748 little-endian encoding. */
int c25519_public(bvm* vm) {
    auto key = bytes(vm, 2, 32);
    auto out = output(vm, 32);
    unsigned char scalar[32];
    c25519_scalar(scalar, key);
    auto n = br_ec_c25519_m15.mulgen(out.data, scalar, 32, BR_EC_curve25519);
    nm_crypto_zero(scalar, sizeof scalar);
    require(vm, n == 32, "invalid Curve25519 key");
    be_return(vm);
}

/** @brief EC_C25519().shared_key(private_key, peer: bytes(32)) -> bytes(32).
 * Reject invalid peers and an all-zero shared secret. */
int c25519_shared(bvm* vm) {
    auto key = bytes(vm, 2, 32), peer = bytes(vm, 3, 32);
    auto out = output(vm, 32);
    unsigned char scalar[32];
    c25519_scalar(scalar, key);
    std::memcpy(out.data, peer.data, 32);
    auto ok = br_ec_c25519_m15.mul(out.data, 32, scalar, 32, BR_EC_curve25519);
    nm_crypto_zero(scalar, sizeof scalar);
    unsigned nonzero = 0;
    for (unsigned i = 0; i < 32; ++i)
        nonzero |= out.data[i];
    if (!ok || !nonzero) {
        nm_crypto_zero(out.data, 32);
        be_raise(vm, "value_error", "invalid Curve25519 peer");
    }
    be_return(vm);
}
#endif
#if USE_BERRY_CRYPTO_CHACHA20_POLY1305
/** @brief CHACHA20_POLY1305.chacha_run(key: bytes(32), nonce: bytes(12), counter: int, data: bytes)
 * -> int. XOR writable data in place; return next 32-bit counter. Reject counter wrap and overlap
 * of data with key/nonce. Signed integers preserve counter bit patterns. */
int chacha_run(bvm* vm) {
    auto key = bytes(vm, 1, 32), iv = bytes(vm, 2, 12);
    auto counter = integer(vm, 3);
    auto data = writable_bytes(vm, 4);
    require(vm, !overlap(key, data) && !overlap(iv, data), "ChaCha20 input overlaps output");
    require(vm,
        (static_cast<uint64_t>(data.size) + 63) / 64 <=
            UINT64_C(0x100000000) - static_cast<uint32_t>(counter),
        "counter overflow");
    auto next =
        br_chacha20_ct_run(key.data, iv.data, static_cast<uint32_t>(counter), data.data, data.size);
    be_pushint(vm, static_cast<bint>(next));
    be_return(vm);
}

/** @brief CHACHA20_POLY1305.poly_run(data: bytes, key: bytes(32)) -> bytes(16).
 * Compute standalone Poly1305 without mutating inputs. */
int poly_run(bvm* vm) {
    auto data = bytes(vm, 1), key = bytes(vm, 2, 32);
    auto out = output(vm, 16);
    require(vm, nm_poly1305(key.data, data.data, data.size, out.data), "invalid Poly1305 data");
    be_return(vm);
}
#endif
#if USE_BERRY_CRYPTO_TEST_BLAKE2S
/** @brief BLAKE2S(length: int, key: bytes): digest length 1..32, key length 0..32. */
int blake_init(bvm* vm) {
    auto n = integer(vm, 2);
    auto key = bytes(vm, 3);
    require(vm, n >= 1 && n <= 32 && key.size <= 32, "invalid BLAKE2s parameters");
    auto* ctx = allocate_context(vm, Context::blake2s);
    require(vm, nm_blake2s_init(&ctx->blake, static_cast<size_t>(n), key.data, key.size),
        "BLAKE2s failed");
    be_return_nil(vm);
}

/** @brief BLAKE2S.update(data: bytes) -> self; append data to the keyed/unkeyed context. */
int blake_update(bvm* vm) {
    auto data = bytes(vm, 2);
    auto* ctx = context(vm, Context::blake2s);
    require(vm, nm_blake2s_update(&ctx->blake, data.data, data.size), "BLAKE2s failed");
    be_pushvalue(vm, 1);
    be_return(vm);
}

/** @brief BLAKE2S.out() -> bytes(length); return a snapshot digest. */
int blake_out(bvm* vm) {
    auto* ctx = context(vm, Context::blake2s);
    auto out = output(vm, ctx->blake.output_len);
    require(vm, nm_blake2s_out(&ctx->blake, out.data), "BLAKE2s failed");
    be_return(vm);
}

/** @brief BLAKE2S.hmac(key: bytes, data: bytes) -> bytes(32); arbitrary key length. */
int blake_hmac(bvm* vm) {
    auto key = bytes(vm, 1), data = bytes(vm, 2);
    auto out = output(vm, 32);
    require(vm, nm_hmac_blake2s(key.data, key.size, data.data, data.size, out.data),
        "HMAC BLAKE2s failed");
    be_return(vm);
}
#endif
#if USE_BERRY_CRYPTO_TEST_XCHACHA20_POLY1305
/** @brief XCHACHA20_POLY1305.run(key: bytes(32), nonce: bytes(24), data, aad: bytes, tag:
 * bytes(16), encrypt: bool) -> bool. Writable data changes in place; encryption writes tag.
 * Decryption preserves ciphertext on authentication failure. Data must not overlap tag or AAD. */
int xchacha_run(bvm* vm) {
    auto key = bytes(vm, 1, 32), nonce = bytes(vm, 2, 24), data = writable_bytes(vm, 3),
         aad = bytes(vm, 4), tag = bytes(vm, 5, 16);
    require(vm, be_isbool(vm, 6), "encrypt must be boolean");
    if (be_tobool(vm, 6))
        tag = writable_bytes(vm, 5, 16);
    require(vm, !overlap(data, tag) && !overlap(data, aad), "XChaCha20 buffers overlap");
    be_pushbool(vm, nm_xchacha20_poly1305(key.data, nonce.data, data.data, data.size, aad.data,
                        aad.size, tag.data, be_tobool(vm, 6)));
    be_return(vm);
}
#endif
#if USE_BERRY_CRYPTO_AES_CTR
/** @brief AES_CTR(key: bytes(32)): initialize AES-256 CTR. */
int ctr_init(bvm* vm) {
    auto key = bytes(vm, 2, 32);
    auto* ctx = allocate_context(vm, Context::ctr);
    br_aes_small_ctr_init(&ctx->aes_ctr, key.data, key.size);
    be_return_nil(vm);
}

/** @brief AES_CTR.encrypt/decrypt(data: bytes, nonce: bytes(12), counter: int) -> bytes.
 * Each call starts at its explicit counter; signed values encode all 32 bits.
 * Reject counter wrap, preserve input and return a same-length copy. */
int ctr_run(bvm* vm) {
    auto data = bytes(vm, 2), iv = bytes(vm, 3, 12);
    auto counter = integer(vm, 4);
    // Berry integers carry all 32 counter bits, including signed representations.
    require(vm,
        (static_cast<uint64_t>(data.size) + 15) / 16 <=
            UINT64_C(0x100000000) - static_cast<uint32_t>(counter),
        "counter overflow");
    auto* ctx = context(vm, Context::ctr);
    auto out = output(vm, data.size);
    if (data.size)
        std::memcpy(out.data, data.data, data.size);
    br_aes_small_ctr_run(
        &ctx->aes_ctr, iv.data, static_cast<uint32_t>(counter), out.data, out.size);
    be_return(vm);
}
#endif
#if USE_BERRY_CRYPTO_AES_GCM
/** @brief AES_GCM(key: bytes(32), iv: bytes): initialize streaming AES-256 GCM without AAD. */
int gcm_init(bvm* vm) {
    auto key = bytes(vm, 2, 32), iv = bytes(vm, 3);
    auto* ctx = allocate_context(vm, Context::gcm);
    br_aes_small_ctr_init(&ctx->aes_ctr, key.data, key.size);
    br_gcm_init(&ctx->gcm_ctx, &ctx->aes_ctr.vtable, br_ghash_ctmul32);
    br_gcm_reset(&ctx->gcm_ctx, iv.data, iv.size);
    br_gcm_flip(&ctx->gcm_ctx);
    be_return_nil(vm);
}

/** @brief AES_GCM.encrypt/decrypt(data: bytes) -> bytes; append a same-length copy.
 * First call fixes direction; switching direction or updating after tag() fails.
 * Streaming decrypt exposes plaintext before authentication: callers must compare
 * tag() with the expected tag before using that plaintext. */
int gcm_run(bvm* vm, bool encrypt) {
    auto data = bytes(vm, 2);
    auto* ctx = context(vm, Context::gcm);
    require(vm, !ctx->finalized, "GCM already finalized");
    require(vm, !ctx->mode_set || ctx->encrypt == encrypt, "GCM direction changed");
    // SP 800-38D limits one invocation to 2^39-256 plaintext bits.
    require(vm,
        static_cast<uint64_t>(data.size) <= UINT64_C(0xFFFFFFFE) * 16 - ctx->gcm_ctx.count_ctr,
        "GCM length exceeded");
    auto out = output(vm, data.size);
    if (data.size)
        std::memcpy(out.data, data.data, data.size);
    ctx->mode_set = true;
    ctx->encrypt = encrypt;
    br_gcm_run(&ctx->gcm_ctx, encrypt, out.data, out.size);
    be_return(vm);
}

/** @brief AES_GCM.encrypt(data: bytes) -> bytes; see gcm_run for streaming state rules. */
int gcm_encrypt(bvm* vm) {
    return gcm_run(vm, true);
}

/** @brief AES_GCM.decrypt(data: bytes) -> bytes; plaintext requires subsequent tag verification. */
int gcm_decrypt(bvm* vm) {
    return gcm_run(vm, false);
}

/** @brief AES_GCM.tag() -> bytes(16); finalize streaming updates and return a repeatable tag. */
int gcm_tag(bvm* vm) {
    auto* ctx = context(vm, Context::gcm);
    auto out = output(vm, 16);
    if (!ctx->finalized) {
        br_gcm_get_tag(&ctx->gcm_ctx, ctx->tag);
        ctx->finalized = true;
    }
    std::memcpy(out.data, ctx->tag, 16);
    be_return(vm);
}
#endif
#if USE_BERRY_CRYPTO_AES_CCM
/** @brief AES_CCM(key: bytes(16|32), nonce: bytes(7..13), aad: bytes, length: int, tag_length:
 * int). Declare total payload length and an even tag length 4..16 before streaming. */
int ccm_init(bvm* vm) {
    auto key = bytes(vm, 2), nonce = bytes(vm, 3), aad = bytes(vm, 4);
    auto length = integer(vm, 5), tag = integer(vm, 6);
    require(vm, key.size == 16 || key.size == 32, "Key size must be 16 or 32 bytes");
    require(vm, length >= 0 && tag >= 4 && tag <= 16 && !(tag & 1), "invalid CCM parameters");
    require(vm, nonce.size >= 7 && nonce.size <= 13, "invalid CCM nonce");
    auto* ctx = allocate_context(vm, Context::ccm);
    br_aes_small_ctrcbc_init(&ctx->aes_ccm, key.data, key.size);
    br_ccm_init(&ctx->ccm_ctx, &ctx->aes_ccm.vtable);
    require(vm,
        br_ccm_reset(&ctx->ccm_ctx, nonce.data, nonce.size, aad.size, static_cast<uint64_t>(length),
            static_cast<size_t>(tag)),
        "invalid CCM parameters");
    br_ccm_aad_inject(&ctx->ccm_ctx, aad.data, aad.size);
    br_ccm_flip(&ctx->ccm_ctx);
    ctx->expected = static_cast<size_t>(length);
    ctx->tag_len = static_cast<size_t>(tag);
    be_return_nil(vm);
}

/** @brief AES_CCM.encrypt/decrypt(data: bytes) -> bytes; preserve input and append a copy.
 * Direction is fixed on first use; reject payload overflow and updates after tag().
 * Streaming decrypt returns unauthenticated plaintext: verify the expected tag
 * before using plaintext. Total bytes must equal the declared length. */
int ccm_run(bvm* vm, bool encrypt) {
    auto data = bytes(vm, 2);
    auto* ctx = context(vm, Context::ccm);
    require(vm, !ctx->finalized, "CCM already finalized");
    require(vm, !ctx->mode_set || ctx->encrypt == encrypt, "CCM direction changed");
    require(vm, data.size <= ctx->expected - ctx->processed, "CCM length exceeded");
    auto out = output(vm, data.size);
    if (data.size)
        std::memcpy(out.data, data.data, data.size);
    ctx->mode_set = true;
    ctx->encrypt = encrypt;
    ctx->processed += data.size;
    br_ccm_run(&ctx->ccm_ctx, encrypt, out.data, out.size);
    be_return(vm);
}

/** @brief AES_CCM.encrypt(data: bytes) -> bytes; see ccm_run for streaming state rules. */
int ccm_encrypt(bvm* vm) {
    return ccm_run(vm, true);
}

/** @brief AES_CCM.decrypt(data: bytes) -> bytes; plaintext requires subsequent tag verification. */
int ccm_decrypt(bvm* vm) {
    return ccm_run(vm, false);
}

/** @brief AES_CCM.tag() -> bytes(16); reject incomplete payloads, finalize and repeat tag reads.
 * Only the configured tag-length prefix is meaningful; compare that prefix. */
int ccm_tag(bvm* vm) {
    auto* ctx = context(vm, Context::ccm);
    require(vm, ctx->processed == ctx->expected, "CCM length incomplete");
    auto out = output(vm, 16);
    if (!ctx->finalized) {
        br_ccm_get_tag(&ctx->ccm_ctx, ctx->tag);
        ctx->finalized = true;
    }
    std::memcpy(out.data, ctx->tag, 16);
    be_return(vm);
}

Bytes slice(bvm* vm, int index) {
    auto data = bytes(vm, index);
    auto start = integer(vm, index + 1), length = integer(vm, index + 2);
    if (!(start >= 0 && length >= 0 && static_cast<size_t>(start) <= data.size &&
            static_cast<size_t>(length) <= data.size - static_cast<size_t>(start)))
        be_raise(vm, "range_error", "out of range start/end");
    return {
        data.data ? data.data + static_cast<size_t>(start) : nullptr, static_cast<size_t>(length)};
}

/** @brief AES_CCM.encrypt1/decrypt1(key, nonce, nonce_start, nonce_len, aad, aad_start, aad_len,
 * data, data_start, data_len, tag, tag_start, tag_len) -> bool. Slices use bytes plus nonnegative
 * start/length; bounds errors raise range_error. Key is 16/32 bytes, nonce 7..13, tag even 4..16.
 * Writable data and tag slices must not overlap. Encryption writes both slices; decrypt
 * authenticates a copy before publishing plaintext and leaves ciphertext unchanged on a false
 * result. */
int ccm_once(bvm* vm, bool encrypt) {
    auto key = bytes(vm, 1), nonce = slice(vm, 2), aad = slice(vm, 5), data = slice(vm, 8),
         tag = slice(vm, 11);
    writable_bytes(vm, 8);
    if (encrypt)
        writable_bytes(vm, 11);
    require(vm, !overlap(data, tag), "CCM data overlaps tag");
    require(vm, key.size == 16 || key.size == 32, "Key size must be 16 or 32 bytes");
    require(vm,
        nonce.size >= 7 && nonce.size <= 13 && tag.size >= 4 && tag.size <= 16 && !(tag.size & 1),
        "invalid CCM parameters");
    // Decrypt a rooted copy and publish it only after tag verification.
    auto copy = output(vm, data.size);
    if (data.size)
        std::memcpy(copy.data, data.data, data.size);
    br_aes_small_ctrcbc_keys aes;
    br_ccm_context ctx;
    br_aes_small_ctrcbc_init(&aes, key.data, key.size);
    br_ccm_init(&ctx, &aes.vtable);
    int ok = br_ccm_reset(&ctx, nonce.data, nonce.size, aad.size, data.size, tag.size);
    if (ok) {
        br_ccm_aad_inject(&ctx, aad.data, aad.size);
        br_ccm_flip(&ctx);
        br_ccm_run(&ctx, encrypt, copy.data, copy.size);
        if (encrypt)
            br_ccm_get_tag(&ctx, tag.data);
        else
            ok = br_ccm_check_tag(&ctx, tag.data) != 0;
        if (ok && data.size)
            std::memcpy(data.data, copy.data, data.size);
    }
    nm_crypto_zero(copy.data, copy.size);
    nm_crypto_zero(&ctx, sizeof ctx);
    nm_crypto_zero(&aes, sizeof aes);
    be_pushbool(vm, ok != 0);
    be_return(vm);
}

/** @brief AES_CCM.encrypt1(...slice arguments...) -> bool; see ccm_once for exact mutation
 * contract. */
int ccm_encrypt1(bvm* vm) {
    return ccm_once(vm, true);
}

/** @brief AES_CCM.decrypt1(...slice arguments...) -> bool; authenticate before writing output. */
int ccm_decrypt1(bvm* vm) {
    return ccm_once(vm, false);
}
#endif
#if USE_BERRY_CRYPTO_AES_CBC
/** @brief AES_CBC.encrypt1/decrypt1(key: bytes(16), iv: bytes(16), data: bytes) -> true.
 * Writable IV and data are updated in place and must not overlap. Data length
 * must be a multiple of 16; no padding is added or removed. */
int cbc_run(bvm* vm, bool encrypt) {
    auto key = bytes(vm, 1, 16), iv = writable_bytes(vm, 2, 16), data = writable_bytes(vm, 3);
    require(vm, data.size % 16 == 0, "CBC length must be a block multiple");
    require(vm, !overlap(iv, data), "CBC data overlaps IV");
    if (encrypt) {
        br_aes_small_cbcenc_keys ctx;
        br_aes_small_cbcenc_init(&ctx, key.data, 16);
        br_aes_small_cbcenc_run(&ctx, iv.data, data.data, data.size);
        nm_crypto_zero(&ctx, sizeof ctx);
    } else {
        br_aes_small_cbcdec_keys ctx;
        br_aes_small_cbcdec_init(&ctx, key.data, 16);
        if (data.size)
            br_aes_small_cbcdec_run(&ctx, iv.data, data.data, data.size);
        nm_crypto_zero(&ctx, sizeof ctx);
    }
    be_pushbool(vm, true);
    be_return(vm);
}

/** @brief AES_CBC.encrypt1(key, iv, data) -> true; see cbc_run for mutation and length contract. */
int cbc_encrypt(bvm* vm) {
    return cbc_run(vm, true);
}

/** @brief AES_CBC.decrypt1(key, iv, data) -> true; see cbc_run for mutation and length contract. */
int cbc_decrypt(bvm* vm) {
    return cbc_run(vm, false);
}
#endif
#if USE_BERRY_CRYPTO_EC_P256
static const unsigned char p256_order[32] = {0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84, 0xf3, 0xb9, 0xca,
    0xc2, 0xfc, 0x63, 0x25, 0x51};

void scalar(bvm* vm, const Bytes& key, bool zero_allowed = false) {
    require(vm, key.size >= 1 && key.size <= 32, "invalid P256 scalar length");
    unsigned char padded[32] = {0};
    std::memcpy(padded + 32 - key.size, key.data, key.size);
    unsigned nonzero = 0;
    for (unsigned i = 0; i < 32; ++i)
        nonzero |= padded[i];
    bool valid = (zero_allowed || nonzero) && std::memcmp(padded, p256_order, 32) < 0;
    nm_crypto_zero(padded, sizeof padded);
    require(vm, valid, "invalid P256 scalar");
}

void valid_point(bvm* vm, const Bytes& p) {
    require(vm, p.size == 65 && p.data[0] == 4, "invalid P256 point");
}

/** @brief EC_P256.public_key(scalar: bytes(1..32)) -> uncompressed bytes(65).
 * Big-endian scalar must be nonzero and less than the curve order. */
int p256_public(bvm* vm) {
    auto key = bytes(vm, 1);
    scalar(vm, key);
    auto out = output(vm, 65);
    auto n = br_ec_p256_m15.mulgen(out.data, key.data, key.size, BR_EC_secp256r1);
    require(vm, n == 65, "P256 key derivation failed");
    be_return(vm);
}

/** @brief EC_P256.shared_key(scalar: bytes(1..32), peer: bytes(65)) -> bytes(32).
 * Validate uncompressed peer and return big-endian shared x coordinate. */
int p256_shared(bvm* vm) {
    auto key = bytes(vm, 1), peer = bytes(vm, 2);
    scalar(vm, key);
    valid_point(vm, peer);
    auto out = output(vm, 32);
    unsigned char p[65];
    std::memcpy(p, peer.data, 65);
    auto ok = br_ec_p256_m15.mul(p, 65, key.data, key.size, BR_EC_secp256r1);
    if (ok)
        std::memcpy(out.data, p + 1, 32);
    nm_crypto_zero(p, sizeof p);
    require(vm, ok, "invalid P256 peer");
    be_return(vm);
}

/** @brief EC_P256.ecdsa_sign_sha256[_asn1](scalar, message: bytes) -> signature bytes.
 * Hash message with SHA-256; deterministic signing returns raw r||s or DER. */
int p256_sign(bvm* vm, bool asn1) {
    auto key = bytes(vm, 1), message = bytes(vm, 2);
    scalar(vm, key);
    // Allocate before placing secrets on the stack, then resize after cleanup.
    auto out = output(vm, asn1 ? 72 : 64);
    unsigned char digest[32], signature[72];
    br_sha256_context hash;
    br_sha256_init(&hash);
    br_sha256_update(&hash, message.data, message.size);
    br_sha256_out(&hash, digest);
    br_ec_private_key private_key = {BR_EC_secp256r1, key.data, key.size};
    size_t n = asn1 ? br_ecdsa_i15_sign_asn1(
                          &br_ec_p256_m15, &br_sha256_vtable, digest, &private_key, signature)
                    : br_ecdsa_i15_sign_raw(
                          &br_ec_p256_m15, &br_sha256_vtable, digest, &private_key, signature);
    if (n && n <= out.size)
        std::memcpy(out.data, signature, n);
    nm_crypto_zero(digest, sizeof digest);
    nm_crypto_zero(signature, sizeof signature);
    nm_crypto_zero(&hash, sizeof hash);
    require(vm, n && n <= out.size, "P256 signing failed");
    if (n != out.size) {
        be_getmember(vm, -1, "resize");
        be_pushvalue(vm, -2);
        be_pushint(vm, static_cast<bint>(n));
        be_call(vm, 2);
        be_pop(vm, 2);
    }
    be_return(vm);
}

/** @brief EC_P256.ecdsa_sign_sha256(scalar, message) -> bytes(64), big-endian r||s. */
int p256_sign_raw(bvm* vm) {
    return p256_sign(vm, false);
}

/** @brief EC_P256.ecdsa_sign_sha256_asn1(scalar, message) -> DER bytes(8..72). */
int p256_sign_asn1(bvm* vm) {
    return p256_sign(vm, true);
}

/** @brief EC_P256.ecdsa_verify_sha256[_asn1](public_key: bytes(65), message, signature: bytes) ->
 * bool. Raw signatures require 64 bytes, DER 8..72; malformed lengths raise value_error. */
int p256_verify(bvm* vm, bool asn1) {
    auto key = bytes(vm, 1), message = bytes(vm, 2), signature = bytes(vm, 3);
    valid_point(vm, key);
    require(vm, asn1 ? (signature.size >= 8 && signature.size <= 72) : signature.size == 64,
        "invalid signature length");
    unsigned char digest[32];
    br_sha256_context hash;
    br_sha256_init(&hash);
    br_sha256_update(&hash, message.data, message.size);
    br_sha256_out(&hash, digest);
    br_ec_public_key public_key = {BR_EC_secp256r1, key.data, key.size};
    auto ok = asn1 ? br_ecdsa_i15_vrfy_asn1(
                         &br_ec_p256_m15, digest, 32, &public_key, signature.data, signature.size)
                   : br_ecdsa_i15_vrfy_raw(
                         &br_ec_p256_m15, digest, 32, &public_key, signature.data, signature.size);
    nm_crypto_zero(digest, sizeof digest);
    nm_crypto_zero(&hash, sizeof hash);
    be_pushbool(vm, ok != 0);
    be_return(vm);
}

/** @brief EC_P256.ecdsa_verify_sha256(public_key, message, signature: bytes(64)) -> bool. */
int p256_verify_raw(bvm* vm) {
    return p256_verify(vm, false);
}

/** @brief EC_P256.ecdsa_verify_sha256_asn1(public_key, message, signature: bytes(8..72)) -> bool.
 */
int p256_verify_asn1(bvm* vm) {
    return p256_verify(vm, true);
}

/** @brief EC_P256.mod/neg(data: nonempty bytes) -> bytes(32).
 * Reduce big-endian data modulo curve order; neg returns order minus residue. */
int p256_modulus(bvm* vm, bool negate) {
    auto data = bytes(vm, 1);
    require(vm, data.size > 0, "data must not be empty");
    auto out = output(vm, 32);
    uint16_t modulus[19], value[19], neg[19];
    br_i15_decode(modulus, p256_order, 32);
    br_i15_decode_reduce(value, data.data, data.size, modulus);
    if (negate) {
        std::memcpy(neg, modulus, sizeof modulus);
        br_i15_sub(neg, value, 1);
        br_i15_encode(out.data, 32, neg);
    } else
        br_i15_encode(out.data, 32, value);
    nm_crypto_zero(modulus, sizeof modulus);
    nm_crypto_zero(value, sizeof value);
    nm_crypto_zero(neg, sizeof neg);
    be_return(vm);
}

/** @brief EC_P256.mod(data: nonempty bytes) -> bytes(32); reduce modulo curve order. */
int p256_mod(bvm* vm) {
    return p256_modulus(vm, false);
}

/** @brief EC_P256.neg(data: nonempty bytes) -> bytes(32); return order minus reduced residue. */
int p256_neg(bvm* vm) {
    return p256_modulus(vm, true);
}

/** @brief EC_P256.mul(scalar: bytes(1..32), point: bytes(65)) -> bytes(65).
 * Validate scalar and point, reject infinity; inputs are preserved. */
int p256_mul(bvm* vm) {
    auto key = bytes(vm, 1), p = bytes(vm, 2);
    scalar(vm, key);
    valid_point(vm, p);
    auto out = output(vm, 65);
    std::memcpy(out.data, p.data, 65);
    require(vm, br_ec_p256_m15.mul(out.data, 65, key.data, key.size, BR_EC_secp256r1),
        "invalid P256 point");
    be_return(vm);
}

/** @brief EC_P256.muladd(x, A, y, B: bytes) -> uncompressed bytes(65).
 * Scalars x/y are 1..32 big-endian bytes, may be zero but must be below order.
 * A/B require valid uncompressed P-256 points; empty B denotes the generator.
 * Reject invalid encoding, out-of-range coordinates, off-curve points and infinity. */
int p256_muladd(bvm* vm) {
    auto x = bytes(vm, 1), a = bytes(vm, 2), y = bytes(vm, 3), b = bytes(vm, 4);
    scalar(vm, x, true);
    scalar(vm, y, true);
    valid_point(vm, a);
    if (b.size)
        valid_point(vm, b);
    auto out = output(vm, 65);
    std::memcpy(out.data, a.data, 65);
    auto ok = br_ec_p256_m15.muladd(
        out.data, b.size ? b.data : nullptr, 65, x.data, x.size, y.data, y.size, BR_EC_secp256r1);
    require(vm, ok, "invalid P256 sum or infinity");
    be_return(vm);
}
#endif
#if USE_BERRY_CRYPTO_RSA
/** @brief RSA.rs256(private_key_der: bytes, message: bytes) -> bytes(modulus length).
 * Decode unencrypted PKCS#1/PKCS#8 RSA, sign SHA-256 with PKCS#1 v1.5 padding.
 * Reject unsupported/malformed keys; GC owns and clears decoder key material. */
int rsa_sign(bvm* vm) {
    auto der = bytes(vm, 1), message = bytes(vm, 2);
    require(vm, der.size > 0, "invalid private key");
    // The decoder retains private key material and uses fixed storage. Attach it
    // to a common object so errors and GC clear it too.
    be_newcomobj(vm, nullptr, [](bvm* v) -> int {
        auto* p = static_cast<br_skey_decoder_context*>(be_tocomptr(v, 1));
        if (p) {
            nm_crypto_zero(p, sizeof *p);
            std::free(p);
        }
        be_return_nil(v);
    });
    auto* decoder =
        static_cast<br_skey_decoder_context*>(std::calloc(1, sizeof(br_skey_decoder_context)));
    if (!decoder)
        be_raise(vm, "memory_error", "RSA decoder allocation failed");
    static_cast<bcommomobj*>(var_toobj(be_indexof(vm, -1)))->data = decoder;
    br_skey_decoder_init(decoder);
    br_skey_decoder_push(decoder, der.data, der.size);
    require(vm,
        !br_skey_decoder_last_error(decoder) && br_skey_decoder_key_type(decoder) == BR_KEYTYPE_RSA,
        "invalid RSA private key");
    const auto* key = br_skey_decoder_get_rsa(decoder);
    auto n = (key->n_bitlen + 7) / 8;
    require(vm, n >= 64 && n <= BR_MAX_RSA_SIZE / 8, "unsupported RSA key size");
    auto out = output(vm, n);
    unsigned char digest[32];
    br_sha256_context hash;
    br_sha256_init(&hash);
    br_sha256_update(&hash, message.data, message.size);
    br_sha256_out(&hash, digest);
    auto ok = br_rsa_i15_pkcs1_sign(BR_HASH_OID_SHA256, digest, 32, key, out.data);
    nm_crypto_zero(digest, sizeof digest);
    nm_crypto_zero(&hash, sizeof hash);
    nm_crypto_zero(decoder, sizeof *decoder);
    require(vm, ok, "RSA signing failed");
    be_return(vm);
}
#endif

#if USE_BERRY_CRYPTO_RANDOM
/** @brief crypto.random(count: int) -> bytes(count), count 0..2048.
 * Delegate to platform-qualified entropy; unavailable entropy raises runtime_error
 * instead of returning a pseudorandom fallback. Zero-byte requests require no entropy. */
int random_bytes(bvm* vm) {
    auto n = integer(vm, 1);
    require(vm, n >= 0 && n <= 2048, "invalid random byte count");
    auto out = output(vm, static_cast<size_t>(n));
    auto result = newmota::platform::crypto_random(out.data, out.size);
    if (result != newmota::platform::CryptoRandomResult::success)
        be_raise(vm, "runtime_error", "cryptographic entropy unavailable");
    be_return(vm);
}
#endif
} // namespace
#if USE_BERRY_CRYPTO_MD5
#include "be_fixed_nm_class_md5.h"
#endif
#if USE_BERRY_CRYPTO_SHA256
#include "be_fixed_nm_class_sha256.h"
#endif
#if USE_BERRY_CRYPTO_HMAC_SHA256
#include "be_fixed_nm_class_hmac_sha256.h"
#endif
#if USE_BERRY_CRYPTO_HKDF_SHA256
#include "be_fixed_nm_class_hkdf_sha256.h"
#endif
#if USE_BERRY_CRYPTO_PBKDF2_HMAC_SHA256
#include "be_fixed_nm_class_pbkdf2_hmac_sha256.h"
#endif
#if USE_BERRY_CRYPTO_ED25519
#include "be_fixed_nm_class_ed25519.h"
#endif
#if USE_BERRY_CRYPTO_EC_C25519
#include "be_fixed_nm_class_ec_c25519.h"
#endif
#if USE_BERRY_CRYPTO_EC_P256
#include "be_fixed_nm_class_ec_p256.h"
#endif
#if USE_BERRY_CRYPTO_CHACHA20_POLY1305
#include "be_fixed_nm_class_chacha20_poly1305.h"
#endif
#if USE_BERRY_CRYPTO_AES_CBC
#include "be_fixed_nm_class_aes_cbc.h"
#endif
#if USE_BERRY_CRYPTO_AES_CTR
#include "be_fixed_nm_class_aes_ctr.h"
#endif
#if USE_BERRY_CRYPTO_AES_GCM
#include "be_fixed_nm_class_aes_gcm.h"
#endif
#if USE_BERRY_CRYPTO_AES_CCM
#include "be_fixed_nm_class_aes_ccm.h"
#endif
#if USE_BERRY_CRYPTO_RSA
#include "be_fixed_nm_class_rsa.h"
#endif
#if USE_BERRY_CRYPTO_TEST_BLAKE2S
#include "be_fixed_nm_class_test_blake2s.h"
#endif
#if USE_BERRY_CRYPTO_TEST_XCHACHA20_POLY1305
#include "be_fixed_nm_class_test_xchacha20_poly1305.h"
#endif
namespace {
/** @brief Resolve a selected crypto member by string name through Berry module lookup.
 * Excluded APIs yield Berry undefined; SPAKE is a solidified class. */
int crypto_member(bvm* vm) {
    if (!be_isstring(vm, 1))
        be_raise(vm, "type_error", "crypto member expects a name");
    const char* name = be_tostring(vm, 1);
#if USE_BERRY_CRYPTO_MD5
    if (std::strcmp(name, "MD5") == 0) {
        be_pushntvclass(vm, &nm_class_md5);
        be_return(vm);
    }
#endif
#if USE_BERRY_CRYPTO_SHA256
    if (std::strcmp(name, "SHA256") == 0) {
        be_pushntvclass(vm, &nm_class_sha256);
        be_return(vm);
    }
#endif
#if USE_BERRY_CRYPTO_HMAC_SHA256
    if (std::strcmp(name, "HMAC_SHA256") == 0) {
        be_pushntvclass(vm, &nm_class_hmac_sha256);
        be_return(vm);
    }
#endif
#if USE_BERRY_CRYPTO_HKDF_SHA256
    if (std::strcmp(name, "HKDF_SHA256") == 0) {
        be_pushntvclass(vm, &nm_class_hkdf_sha256);
        be_return(vm);
    }
#endif
#if USE_BERRY_CRYPTO_PBKDF2_HMAC_SHA256
    if (std::strcmp(name, "PBKDF2_HMAC_SHA256") == 0) {
        be_pushntvclass(vm, &nm_class_pbkdf2_hmac_sha256);
        be_return(vm);
    }
#endif
#if USE_BERRY_CRYPTO_ED25519
    if (std::strcmp(name, "ED25519") == 0) {
        be_pushntvclass(vm, &nm_class_ed25519);
        be_return(vm);
    }
#endif
#if USE_BERRY_CRYPTO_EC_C25519
    if (std::strcmp(name, "EC_C25519") == 0) {
        be_pushntvclass(vm, &nm_class_ec_c25519);
        be_return(vm);
    }
#endif
#if USE_BERRY_CRYPTO_EC_P256
    if (std::strcmp(name, "EC_P256") == 0) {
        be_pushntvclass(vm, &nm_class_ec_p256);
        be_return(vm);
    }
#endif
#if USE_BERRY_CRYPTO_CHACHA20_POLY1305
    if (std::strcmp(name, "CHACHA20_POLY1305") == 0) {
        be_pushntvclass(vm, &nm_class_chacha20_poly1305);
        be_return(vm);
    }
#endif
#if USE_BERRY_CRYPTO_AES_CBC
    if (std::strcmp(name, "AES_CBC") == 0) {
        be_pushntvclass(vm, &nm_class_aes_cbc);
        be_return(vm);
    }
#endif
#if USE_BERRY_CRYPTO_AES_CTR
    if (std::strcmp(name, "AES_CTR") == 0) {
        be_pushntvclass(vm, &nm_class_aes_ctr);
        be_return(vm);
    }
#endif
#if USE_BERRY_CRYPTO_AES_GCM
    if (std::strcmp(name, "AES_GCM") == 0) {
        be_pushntvclass(vm, &nm_class_aes_gcm);
        be_return(vm);
    }
#endif
#if USE_BERRY_CRYPTO_AES_CCM
    if (std::strcmp(name, "AES_CCM") == 0) {
        be_pushntvclass(vm, &nm_class_aes_ccm);
        be_return(vm);
    }
#endif
#if USE_BERRY_CRYPTO_RSA
    if (std::strcmp(name, "RSA") == 0) {
        be_pushntvclass(vm, &nm_class_rsa);
        be_return(vm);
    }
#endif
#if USE_BERRY_CRYPTO_TEST_BLAKE2S
    if (std::strcmp(name, "BLAKE2S") == 0) {
        be_pushntvclass(vm, &nm_class_test_blake2s);
        be_return(vm);
    }
#endif
#if USE_BERRY_CRYPTO_TEST_XCHACHA20_POLY1305
    if (std::strcmp(name, "XCHACHA20_POLY1305") == 0) {
        be_pushntvclass(vm, &nm_class_test_xchacha20_poly1305);
        be_return(vm);
    }
#endif
#if USE_BERRY_CRYPTO_SPAKE2P_MATTER
    if (std::strcmp(name, "SPAKE2P_Matter") == 0) {
        be_pushntvclass(vm, &be_class_SPAKE2P_Matter);
        be_return(vm);
    }
#endif
    be_getmodule(vm, "undefined");
    be_return(vm);
}
} // namespace

extern "C" {
be_native_module_attr_table(crypto){
    be_native_module_function("member", crypto_member),
#if USE_BERRY_CRYPTO_RANDOM
    be_native_module_function("random", random_bytes),
#endif
};
extern const bntvmodule_t be_native_module_crypto = {
    "crypto", crypto_attrs, sizeof(crypto_attrs) / sizeof(crypto_attrs[0]), nullptr};
}
// clang-format off
/* @const_object_info_begin
class nm_class_md5 (scope: global, name: MD5, depend: USE_BERRY_CRYPTO_MD5) {
    .p, var
    init, func(md5_init)
    update, func(md5_update)
    finish, func(md5_finish)
}

class nm_class_sha256 (scope: global, name: SHA256, depend: USE_BERRY_CRYPTO_SHA256) {
    .p, var
    init, func(sha_init)
    update, func(sha_update)
    out, func(sha_out)
}

class nm_class_hmac_sha256 (scope: global, name: HMAC_SHA256, depend: USE_BERRY_CRYPTO_HMAC_SHA256) {
    .p, var
    init, func(hmac_init)
    update, func(hmac_update)
    out, func(hmac_out)
}

class nm_class_hkdf_sha256 (scope: global, name: HKDF_SHA256, depend: USE_BERRY_CRYPTO_HKDF_SHA256) {
    derive, static_func(hkdf_derive)
}

class nm_class_pbkdf2_hmac_sha256 (scope: global, name: PBKDF2_HMAC_SHA256, depend: USE_BERRY_CRYPTO_PBKDF2_HMAC_SHA256) {
    derive, func(pbkdf2_derive)
    _f, static_func(pbkdf2_f)
}

class nm_class_ed25519 (scope: global, name: ED25519, depend: USE_BERRY_CRYPTO_ED25519) {
    secret_key, func(ed_secret)
    sign, func(ed_sign)
    verify, func(ed_verify)
}

class nm_class_ec_c25519 (scope: global, name: EC_C25519, depend: USE_BERRY_CRYPTO_EC_C25519) {
    public_key, func(c25519_public)
    shared_key, func(c25519_shared)
}

class nm_class_ec_p256 (scope: global, name: EC_P256, depend: USE_BERRY_CRYPTO_EC_P256) {
    public_key, static_func(p256_public)
    shared_key, static_func(p256_shared)
    ecdsa_sign_sha256, static_func(p256_sign_raw)
    ecdsa_sign_sha256_asn1, static_func(p256_sign_asn1)
    ecdsa_verify_sha256, static_func(p256_verify_raw)
    ecdsa_verify_sha256_asn1, static_func(p256_verify_asn1)
    mod, static_func(p256_mod)
    neg, static_func(p256_neg)
    mul, static_func(p256_mul)
    muladd, static_func(p256_muladd)
}

class nm_class_chacha20_poly1305 (scope: global, name: CHACHA20_POLY1305, depend: USE_BERRY_CRYPTO_CHACHA20_POLY1305) {
    chacha_run, static_func(chacha_run)
    poly_run, static_func(poly_run)
}

class nm_class_aes_cbc (scope: global, name: AES_CBC, depend: USE_BERRY_CRYPTO_AES_CBC) {
    encrypt1, static_func(cbc_encrypt)
    decrypt1, static_func(cbc_decrypt)
}

class nm_class_aes_ctr (scope: global, name: AES_CTR, depend: USE_BERRY_CRYPTO_AES_CTR) {
    .p, var
    init, func(ctr_init)
    encrypt, func(ctr_run)
    decrypt, func(ctr_run)
}

class nm_class_aes_gcm (scope: global, name: AES_GCM, depend: USE_BERRY_CRYPTO_AES_GCM) {
    .p, var
    init, func(gcm_init)
    encrypt, func(gcm_encrypt)
    decrypt, func(gcm_decrypt)
    tag, func(gcm_tag)
}

class nm_class_aes_ccm (scope: global, name: AES_CCM, depend: USE_BERRY_CRYPTO_AES_CCM) {
    .p, var
    init, func(ccm_init)
    encrypt, func(ccm_encrypt)
    decrypt, func(ccm_decrypt)
    tag, func(ccm_tag)
    encrypt1, static_func(ccm_encrypt1)
    decrypt1, static_func(ccm_decrypt1)
}

class nm_class_rsa (scope: global, name: RSA, depend: USE_BERRY_CRYPTO_RSA) {
    rs256, static_func(rsa_sign)
}

class nm_class_test_blake2s (scope: global, name: BLAKE2S, depend: USE_BERRY_CRYPTO_TEST_BLAKE2S) {
    .p, var
    init, func(blake_init)
    update, func(blake_update)
    out, func(blake_out)
    hmac, static_func(blake_hmac)
}

class nm_class_test_xchacha20_poly1305 (scope: global, name: XCHACHA20_POLY1305, depend: USE_BERRY_CRYPTO_TEST_XCHACHA20_POLY1305) {
    run, static_func(xchacha_run)
}

@const_object_info_end */
// clang-format on
