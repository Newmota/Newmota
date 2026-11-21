/* Copyright (C) 2026 Newmota contributors. SPDX-License-Identifier: GPL-3.0-or-later */
#include "crypto_kat.h"
#include "newmota_crypto.h"

int crypto_extension_kat() {
    using namespace crypto_kat;
    Report r;
    Bytes out(32);
    // RFC 8032 section 7.1, test 1 (empty message).
    auto seed = from_hex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    Bytes secret(64), signature(64);
    r.expect(nm_ed25519_secret_key(seed.data(), secret.data()), "Ed25519 seed derivation");
    r.expect_hex(secret,
        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
        "Ed25519 seed || public layout");
    r.expect(nm_ed25519_sign(secret.data(), nullptr, 0, signature.data()), "Ed25519 sign empty");
    r.expect_hex(signature,
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
        "RFC 8032 signature");
    r.expect(nm_ed25519_verify(secret.data() + 32, nullptr, 0, signature.data()), "Ed25519 verify");
    signature[0] ^= 1;
    r.expect(!nm_ed25519_verify(secret.data() + 32, nullptr, 0, signature.data()),
        "Ed25519 reject corrupt signature");
    signature[0] ^= 1;
    secret[32] ^= 1;
    r.expect(!nm_ed25519_sign(secret.data(), nullptr, 0, signature.data()),
        "Ed25519 rejects inconsistent secret key");
    r.expect(!nm_ed25519_sign(nullptr, nullptr, 0, signature.data()), "Ed25519 rejects null key");
    // RFC 8032 test 2: one-byte message.
    seed = from_hex("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb");
    auto ed_message = from_hex("72");
    nm_ed25519_secret_key(seed.data(), secret.data());
    r.expect(nm_ed25519_sign(secret.data(), ed_message.data(), ed_message.size(), signature.data()),
        "Ed25519 sign nonempty");
    r.expect_hex(signature,
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
        "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00",
        "RFC 8032 nonempty signature");
    r.expect(nm_ed25519_verify(
                 secret.data() + 32, ed_message.data(), ed_message.size(), signature.data()),
        "Ed25519 verify nonempty");
    ed_message[0] ^= 1;
    r.expect(!nm_ed25519_verify(
                 secret.data() + 32, ed_message.data(), ed_message.size(), signature.data()),
        "Ed25519 rejects altered message");
    const auto saved_signature = signature;
    auto noncanonical_s =
        from_hex("edd3f55c1a631258d69cf7a2def9de1400000000000000000000000000000010");
    std::memcpy(signature.data() + 32, noncanonical_s.data(), 32);
    r.expect(!nm_ed25519_verify(secret.data() + 32, nullptr, 0, signature.data()),
        "Ed25519 rejects S equal to order");
    signature = saved_signature;
    Bytes identity_key(32);
    identity_key[0] = 1;
    r.expect(!nm_ed25519_verify(identity_key.data(), nullptr, 0, signature.data()),
        "Ed25519 rejects identity public key");
    auto noncanonical_y =
        from_hex("edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f");
    r.expect(!nm_ed25519_verify(noncanonical_y.data(), nullptr, 0, signature.data()),
        "Ed25519 rejects y equal to prime");
    // RFC 7914 section 11 (PBKDF2-HMAC-SHA256, two output blocks).
    out.resize(64);
    r.expect(
        nm_pbkdf2_sha256("passwd", 6, "salt", 4, 1, out.data(), out.size()), "PBKDF2 accepted");
    r.expect_hex(out,
        "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc4"
        "9ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783",
        "RFC 7914 PBKDF2");
    const Bytes preserved = out;
    r.expect(
        !nm_pbkdf2_sha256("p", 1, "s", 1, 0, out.data(), out.size()), "reject zero iterations");
    r.expect(out == preserved, "PBKDF2 failure preserves output");
    out.resize(32);
    r.expect(nm_pbkdf2_sha256("Password", 8, "NaCl", 4, 80000, out.data(), out.size()),
        "iterated PBKDF2");
    r.expect_hex(out, "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56",
        "RFC 7914 iterated prefix");

    // Official BLAKE2 reference KATs:
    // https://github.com/BLAKE2/BLAKE2/blob/master/testvectors/blake2-kat.json
    const size_t lengths[] = {0, 1, 63, 64, 65, 255};
    const char* expected[] = {"48a8997da407876b3d79c0d92325ad3b89cbb754d86ab71aee047ad345fd2c49",
        "40d15fee7c328830166ac3f918650f807e7e01e177258cdc0a39b11f598066f1",
        "c65382513f07460da39833cb666c5ed82e61b9e998f4b0c4287cee56c3cc9bcd",
        "8975b0577fd35566d750b362b0897a26c399136df07bababbde6203ff2954ed4",
        "21fe0ceb0052be7fb0f004187cacd7de67fa6eb0938d927677f2398c132317a8",
        "3fb735061abc519dfe979e54c1ee5bfad0a9d858b3315bad34bde999efd724dd"};
    Bytes input(255), key(32);
    for (size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<uint8_t>(i);
    for (size_t i = 0; i < key.size(); ++i)
        key[i] = static_cast<uint8_t>(i);
    nm_blake2s_context ctx{}, interleaved{};
    r.expect(nm_blake2s_init(&interleaved, 32, nullptr, 0), "BLAKE2s empty init");
    for (size_t i = 0; i < 6; ++i) {
        r.expect(nm_blake2s_init(&ctx, 32, key.data(), key.size()), "BLAKE2s keyed init");
        for (size_t j = 0; j < lengths[i]; ++j)
            r.expect(nm_blake2s_update(&ctx, input.data() + j, 1), "BLAKE2s chunk");
        nm_blake2s_out(&ctx, out.data());
        r.expect_hex(out, expected[i], "BLAKE2s keyed vector");
        nm_blake2s_out(&ctx, out.data());
        r.expect_hex(out, expected[i], "BLAKE2s repeatable output");
    }
    nm_blake2s_out(&interleaved, out.data());
    r.expect_hex(out, "69217a3079908094e11121d042354a7c1f55b6482ca1a51e1b250dfd1ed0eef9",
        "BLAKE2s empty interleaved");
    nm_blake2s_init(&ctx, 32, nullptr, 0);
    nm_blake2s_update(&ctx, "abc", 3);
    nm_blake2s_out(&ctx, out.data());
    r.expect_hex(
        out, "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982", "RFC 7693 abc");
    r.expect(!nm_blake2s_init(&ctx, 0, nullptr, 0), "BLAKE2s rejects zero output");
    r.expect(!nm_blake2s_init(&ctx, 33, nullptr, 0), "BLAKE2s rejects oversized output");
    r.expect(!nm_blake2s_init(&ctx, 32, key.data(), 33), "BLAKE2s rejects oversized key");
    nm_blake2s_init(&ctx, 32, nullptr, 0);
    ctx.count = UINT64_MAX - 63;
    ctx.buffered = 63;
    const auto before_overflow = ctx;
    r.expect(!nm_blake2s_update(&ctx, "x", 1), "BLAKE2s rejects counter overflow");
    r.expect(
        std::memcmp(&ctx, &before_overflow, sizeof ctx) == 0, "BLAKE2s overflow preserves context");
    ctx.buffered = 65;
    r.expect(!nm_blake2s_out(&ctx, out.data()), "BLAKE2s rejects invalid buffered size");
    r.expect(!nm_blake2s_update(nullptr, "x", 1), "BLAKE2s rejects null context");
    r.expect(!nm_hmac_blake2s(nullptr, 1, nullptr, 0, out.data()), "HMAC rejects null key");
    out.resize(16);
    nm_blake2s_init(&ctx, 16, "key", 3);
    nm_blake2s_update(&ctx, "abc", 3);
    nm_blake2s_out(&ctx, out.data());
    // Independently generated with Python hashlib.blake2s (RFC 7693 parameters).
    r.expect_hex(out, "94fdf6f35b9999920dcdcaee361ad435", "BLAKE2s truncated keyed output");
    out.resize(32);
    nm_hmac_blake2s("key", 3, "The quick brown fox jumps over the lazy dog", 43, out.data());
    // Independent Python hmac/hashlib oracle; this is HMAC, not BLAKE2 keyed mode.
    r.expect_hex(
        out, "f93215bb90d4af4c3061cd932fb169fb8bb8a91d0b4022baea1271e1323cd9a0", "HMAC BLAKE2s");

    // RFC 8439 section 2.5.2, raw MAC with a partial final block.
    auto polykey = from_hex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b");
    out.resize(16);
    nm_poly1305(polykey.data(), "Cryptographic Forum Research Group", 34, out.data());
    r.expect_hex(out, "a8061dc1305136c6c22b8baf0c0127a9", "RFC 8439 raw Poly1305");
    nm_poly1305(polykey.data(), nullptr, 0, out.data());
    r.expect_hex(out, "0103808afb0db2fd4abff6af4149f51b", "empty raw Poly1305");
    r.expect(
        !nm_poly1305(polykey.data(), nullptr, 1, out.data()), "raw Poly1305 rejects null payload");
    r.expect(!nm_poly1305(nullptr, nullptr, 0, out.data()), "raw Poly1305 rejects null key");

    // draft-irtf-cfrg-xchacha-03 section 2.2.1.
    auto nonce = from_hex("000000090000004a0000000031415927");
    out.resize(32);
    nm_hchacha20(key.data(), nonce.data(), out.data());
    r.expect_hex(out, "82413b4227b27bfed30e42508a877d73a0f9e4d58a74a853c12ec41326d3ecdc",
        "HChaCha20 published vector");
    r.expect(!nm_hchacha20(nullptr, nonce.data(), out.data()), "HChaCha rejects null key");
    // Same draft, appendix A.3.1: XChaCha20-Poly1305 encryption.
    key = from_hex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
    nonce = from_hex("404142434445464748494a4b4c4d4e4f5051525354555657");
    auto aad = from_hex("50515253c0c1c2c3c4c5c6c7");
    const char* message = "Ladies and Gentlemen of the class of '99: If I could offer you only one "
                          "tip for the future, sunscreen would be it.";
    Bytes payload(message, message + std::strlen(message)), tag(16);
    r.expect(nm_xchacha20_poly1305(key.data(), nonce.data(), payload.data(), payload.size(),
                 aad.data(), aad.size(), tag.data(), 1),
        "XChaCha encrypt");
    r.expect_hex(payload,
        "bd6d179d3e83d43b9576579493c0e939572a1700252bfaccbed2902c21396cbb"
        "731c7f1b0b4aa6440bf3a82f4eda7e39ae64c6708c54c216cb96b72e1213b452"
        "2f8c9ba40db5d945b11b69b982c1bb9e3f3fac2bc369488f76b2383565d3fff9"
        "21f9664c97637da9768812f615c68b13b52e",
        "XChaCha ciphertext");
    r.expect_hex(tag, "c0875924c1c7987947deafd8780acf49", "XChaCha tag");
    const auto ciphertext = payload;
    tag[0] ^= 1;
    r.expect(!nm_xchacha20_poly1305(key.data(), nonce.data(), payload.data(), payload.size(),
                 aad.data(), aad.size(), tag.data(), 0),
        "reject wrong XChaCha tag");
    r.expect(payload == ciphertext, "failed authentication preserves ciphertext");
    tag[0] ^= 1;
    r.expect(nm_xchacha20_poly1305(key.data(), nonce.data(), payload.data(), payload.size(),
                 aad.data(), aad.size(), tag.data(), 0),
        "XChaCha decrypt");
    r.expect(
        payload == Bytes(message, message + std::strlen(message)), "XChaCha recovered payload");
    nm_crypto_zero(&ctx, sizeof ctx);
    nm_crypto_zero(&interleaved, sizeof interleaved);
    return r.failures();
}
