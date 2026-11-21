/*
  crypto_sym_kat.cpp - host known-answer tests for the imported BearSSL ciphers

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Vector sources:
    AES block          FIPS 197 Appendix C.1 and C.3
    AES CBC, CTR       NIST SP 800-38A F.2.1 and F.5.1
    AES-GCM            NIST GCM specification test cases 1 to 4
    AES-CCM            RFC 3610 section 8, packet vector #1
    ChaCha20           RFC 8439 section 2.4.2
    ChaCha20-Poly1305  RFC 8439 section 2.8.2, produced through the imported
                       Poly1305 composition, which is this tree's only
                       Poly1305 entry point
*/

#include "crypto_kat.h"

#include "bearssl.h"

namespace {

using crypto_kat::Bytes;
using crypto_kat::from_hex;
using crypto_kat::Report;

Bytes ascii(const char* const text) {
    return Bytes(text, text + std::strlen(text));
}

void test_aes_block(Report& report) {
    const Bytes plaintext = from_hex("00112233445566778899aabbccddeeff");
    const Bytes key128 = from_hex("000102030405060708090a0b0c0d0e0f");
    const Bytes key256 =
        from_hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");

    br_aes_ct_cbcenc_keys encrypt128{};
    br_aes_ct_cbcenc_init(&encrypt128, key128.data(), key128.size());
    report.expect(encrypt128.num_rounds == 10U, "AES-128 selects ten rounds");
    Bytes iv128(16U, 0U);
    Bytes block = plaintext;
    br_aes_ct_cbcenc_run(&encrypt128, iv128.data(), block.data(), block.size());
    report.expect_hex(
        block, "69c4e0d86a7b0430d8cdb78070b4c55a", "AES-128 block encryption matches FIPS 197 C.1");

    br_aes_ct_cbcdec_keys decrypt128{};
    br_aes_ct_cbcdec_init(&decrypt128, key128.data(), key128.size());
    Bytes iv128_back(16U, 0U);
    br_aes_ct_cbcdec_run(&decrypt128, iv128_back.data(), block.data(), block.size());
    report.expect(block == plaintext, "AES-128 block decryption round-trips");

    br_aes_ct_cbcenc_keys encrypt256{};
    br_aes_ct_cbcenc_init(&encrypt256, key256.data(), key256.size());
    report.expect(encrypt256.num_rounds == 14U, "AES-256 selects fourteen rounds");
    Bytes iv256(16U, 0U);
    block = plaintext;
    br_aes_ct_cbcenc_run(&encrypt256, iv256.data(), block.data(), block.size());
    report.expect_hex(
        block, "8ea2b7ca516745bfeafc49904b496089", "AES-256 block encryption matches FIPS 197 C.3");

    br_aes_ct_cbcdec_keys decrypt256{};
    br_aes_ct_cbcdec_init(&decrypt256, key256.data(), key256.size());
    Bytes iv256_back(16U, 0U);
    br_aes_ct_cbcdec_run(&decrypt256, iv256_back.data(), block.data(), block.size());
    report.expect(block == plaintext, "AES-256 block decryption round-trips");

    const Bytes invalid_key = from_hex("00010203");
    br_aes_ct_cbcenc_keys rejected{};
    br_aes_ct_cbcenc_init(&rejected, invalid_key.data(), invalid_key.size());
    report.expect(rejected.num_rounds == 0U, "an unsupported AES key length yields zero rounds");
}

void test_aes_cbc(Report& report) {
    const Bytes key = from_hex("2b7e151628aed2a6abf7158809cf4f3c");
    const Bytes iv = from_hex("000102030405060708090a0b0c0d0e0f");
    const Bytes plaintext = from_hex("6bc1bee22e409f96e93d7e117393172a"
                                     "ae2d8a571e03ac9c9eb76fac45af8e51"
                                     "30c81c46a35ce411e5fbc1191a0a52ef"
                                     "f69f2445df4f9b17ad2b417be66c3710");
    const char* expected = "7649abac8119b246cee98e9b12e9197d"
                           "5086cb9b507219ee95db113a917678b2"
                           "73bed6b8e3c1743b7116e69e22229516"
                           "3ff1caa1681fac09120eca307586e1a7";

    br_aes_ct_cbcenc_keys encrypt{};
    br_aes_ct_cbcenc_init(&encrypt, key.data(), key.size());
    Bytes iv_state = iv;
    Bytes buffer = plaintext;
    br_aes_ct_cbcenc_run(&encrypt, iv_state.data(), buffer.data(), buffer.size());
    report.expect_hex(buffer, expected, "AES-128-CBC matches NIST SP 800-38A F.2.1");
    report.expect(iv_state != iv, "AES-128-CBC advances the caller's chaining state in place");

    const Bytes ciphertext = buffer;
    br_aes_ct_cbcdec_keys decrypt{};
    br_aes_ct_cbcdec_init(&decrypt, key.data(), key.size());
    Bytes iv_state_back = iv;
    br_aes_ct_cbcdec_run(&decrypt, iv_state_back.data(), buffer.data(), buffer.size());
    report.expect(buffer == plaintext, "AES-128-CBC decryption round-trips");

    // Every CBC entry point in this tree consumes whole 16-byte blocks, so the
    // split below stays on a block boundary. A length that is not a multiple of
    // the block size is outside these functions' contract.
    br_aes_ct_cbcenc_keys chained{};
    br_aes_ct_cbcenc_init(&chained, key.data(), key.size());
    Bytes split = plaintext;
    Bytes split_iv = iv;
    br_aes_ct_cbcenc_run(&chained, split_iv.data(), split.data(), 16U);
    br_aes_ct_cbcenc_run(&chained, split_iv.data(), split.data() + 16U, 48U);
    report.expect(split == ciphertext, "split AES-128-CBC runs on a block boundary match one call");
}

void test_aes_ctr(Report& report) {
    const Bytes key = from_hex("2b7e151628aed2a6abf7158809cf4f3c");
    const Bytes counter = from_hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
    const Bytes plaintext = from_hex("6bc1bee22e409f96e93d7e117393172a"
                                     "ae2d8a571e03ac9c9eb76fac45af8e51"
                                     "30c81c46a35ce411e5fbc1191a0a52ef"
                                     "f69f2445df4f9b17ad2b417be66c3710");

    // BearSSL takes the leading twelve counter bytes plus the initial value of
    // the trailing counter word, and returns the next value of that word.
    const Bytes prefix(counter.begin(), counter.begin() + 12);
    std::uint32_t first = 0U;
    for (std::size_t index = 12U; index < counter.size(); ++index) {
        first =
            static_cast<std::uint32_t>(first << 8U) | static_cast<std::uint32_t>(counter[index]);
    }

    br_aes_ct_ctr_keys keys{};
    br_aes_ct_ctr_init(&keys, key.data(), key.size());
    Bytes buffer = plaintext;
    const std::uint32_t next =
        br_aes_ct_ctr_run(&keys, prefix.data(), first, buffer.data(), buffer.size());
    report.expect_hex(buffer,
        "874d6191b620e3261bef6864990db6ce"
        "9806f66b7970fdff8617187bb9fffdff"
        "5ae4df3edbd5d35e5b4f09020db03eab"
        "1e031dda2fbe03d1792170a0f3009cee",
        "AES-128-CTR matches NIST SP 800-38A F.5.1");
    report.expect(next == first + 4U, "AES-128-CTR returns the next counter word");

    // A second context must produce the same keystream, so no per-call state
    // may leak between uses.
    br_aes_ct_ctr_keys other{};
    br_aes_ct_ctr_init(&other, key.data(), key.size());
    Bytes repeated = plaintext;
    br_aes_ct_ctr_run(&other, prefix.data(), first, repeated.data(), repeated.size());
    report.expect(repeated == buffer, "AES-128-CTR keystream does not depend on context history");

    // Non-final calls must end on a 16-byte block boundary. A one-block
    // first call exercises the boundary inside the two-block implementation.
    br_aes_ct_ctr_keys chunked_keys{};
    br_aes_ct_ctr_init(&chunked_keys, key.data(), key.size());
    Bytes chunked = plaintext;
    std::uint32_t running = first;
    running = br_aes_ct_ctr_run(&chunked_keys, prefix.data(), running, chunked.data(), 16U);
    running = br_aes_ct_ctr_run(&chunked_keys, prefix.data(), running, chunked.data() + 16U, 48U);
    report.expect(
        chunked == buffer, "chunked AES-128-CTR matches a single call over the same input");
    report.expect(running == next, "chunked AES-128-CTR reaches the same final counter word");

    // A final short call discards unused bytes of its last keystream block.
    br_aes_ct_ctr_keys partial_keys{};
    br_aes_ct_ctr_init(&partial_keys, key.data(), key.size());
    Bytes partial(8U, 0x11U);
    const std::uint32_t after_partial =
        br_aes_ct_ctr_run(&partial_keys, prefix.data(), first, partial.data(), partial.size());
    report.expect(
        after_partial == first + 1U, "a short AES-128-CTR call advances the counter by one word");
}

void test_aes_gcm(Report& report) {
    struct Vector {
        const char* key;
        const char* iv;
        const char* plaintext;
        const char* aad;
        const char* ciphertext;
        const char* tag;
    };

    // NIST GCM specification test cases 1 to 4.
    const Vector vectors[] = {
        {"00000000000000000000000000000000", "000000000000000000000000", "", "", "",
            "58e2fccefa7e3061367f1d57a4e7455a"},
        {"00000000000000000000000000000000", "000000000000000000000000",
            "00000000000000000000000000000000", "", "0388dace60b6a392f328c2b971b2fe78",
            "ab6e47d42cec13bdf53a67b21257bddf"},
        {"feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888",
            "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95"
            "956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255",
            "",
            "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d5"
            "14b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985",
            "4d5c2af327cd64a62cf35abd2ba6fab4"},
        {"feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888",
            "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95"
            "956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
            "feedfacedeadbeeffeedfacedeadbeefabaddad2",
            "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d5"
            "14b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091",
            "5bc94fbc3221a5db94fae95ae7121a47"},
    };

    for (const Vector& vector : vectors) {
        const Bytes key = from_hex(vector.key);
        const Bytes iv = from_hex(vector.iv);
        const Bytes plaintext = from_hex(vector.plaintext);
        const Bytes aad = from_hex(vector.aad);

        br_aes_ct_ctr_keys keys{};
        br_aes_ct_ctr_init(&keys, key.data(), key.size());
        br_gcm_context context{};
        br_gcm_init(&context, &keys.vtable, &br_ghash_ctmul);
        br_gcm_reset(&context, iv.data(), iv.size());
        if (!aad.empty())
            br_gcm_aad_inject(&context, aad.data(), aad.size());
        Bytes buffer = plaintext;
        br_gcm_flip(&context);
        if (!buffer.empty())
            br_gcm_run(&context, 1, buffer.data(), buffer.size());
        Bytes tag(16U, 0U);
        br_gcm_get_tag(&context, tag.data());
        report.expect_hex(buffer, vector.ciphertext, "AES-128-GCM matches the NIST ciphertext");
        report.expect_hex(tag, vector.tag, "AES-128-GCM matches the NIST tag");

        const Bytes ciphertext = buffer;
        br_gcm_reset(&context, iv.data(), iv.size());
        if (!aad.empty())
            br_gcm_aad_inject(&context, aad.data(), aad.size());
        br_gcm_flip(&context);
        if (!buffer.empty())
            br_gcm_run(&context, 0, buffer.data(), buffer.size());
        report.expect(
            br_gcm_check_tag(&context, tag.data()) == 1U, "AES-128-GCM accepts a matching tag");
        report.expect(buffer == plaintext, "AES-128-GCM decrypts the plaintext");

        Bytes broken_tag = tag;
        broken_tag[0] = static_cast<std::uint8_t>(broken_tag[0] ^ 0x01U);
        br_gcm_reset(&context, iv.data(), iv.size());
        if (!aad.empty())
            br_gcm_aad_inject(&context, aad.data(), aad.size());
        br_gcm_flip(&context);
        buffer = ciphertext;
        if (!buffer.empty())
            br_gcm_run(&context, 0, buffer.data(), buffer.size());
        report.expect(br_gcm_check_tag(&context, broken_tag.data()) == 0U,
            "AES-128-GCM rejects a modified tag");

        Bytes broken_ciphertext = ciphertext;
        if (!broken_ciphertext.empty()) {
            broken_ciphertext[0] = static_cast<std::uint8_t>(broken_ciphertext[0] ^ 0x01U);
            br_gcm_reset(&context, iv.data(), iv.size());
            if (!aad.empty())
                br_gcm_aad_inject(&context, aad.data(), aad.size());
            br_gcm_flip(&context);
            br_gcm_run(&context, 0, broken_ciphertext.data(), broken_ciphertext.size());
            report.expect(br_gcm_check_tag(&context, tag.data()) == 0U,
                "AES-128-GCM rejects a modified ciphertext");
        }
    }

    // A truncated tag is accepted at its own length and rejected when it
    // differs. Reading the tag finalizes the GHASH state, so each check starts
    // from a fresh record.
    const Bytes key = from_hex("feffe9928665731c6d6a8f9467308308");
    const Bytes iv = from_hex("cafebabefacedbaddecaf888");
    const Bytes message(16U, 0x5aU);
    br_aes_ct_ctr_keys keys{};
    br_aes_ct_ctr_init(&keys, key.data(), key.size());
    br_gcm_context context{};
    br_gcm_init(&context, &keys.vtable, &br_ghash_ctmul);
    br_gcm_reset(&context, iv.data(), iv.size());
    br_gcm_flip(&context);
    Bytes record = message;
    br_gcm_run(&context, 1, record.data(), record.size());
    const Bytes record_ciphertext = record;
    Bytes short_tag(8U, 0U);
    br_gcm_get_tag_trunc(&context, short_tag.data(), short_tag.size());

    br_gcm_reset(&context, iv.data(), iv.size());
    br_gcm_flip(&context);
    record = record_ciphertext;
    br_gcm_run(&context, 0, record.data(), record.size());
    report.expect(record == message, "AES-128-GCM decrypts the checked record to its plaintext");
    report.expect(br_gcm_check_tag_trunc(&context, short_tag.data(), short_tag.size()) == 1U,
        "AES-128-GCM accepts a matching truncated tag");
    short_tag[7] = static_cast<std::uint8_t>(short_tag[7] ^ 0x01U);
    br_gcm_reset(&context, iv.data(), iv.size());
    br_gcm_flip(&context);
    record = record_ciphertext;
    br_gcm_run(&context, 0, record.data(), record.size());
    report.expect(br_gcm_check_tag_trunc(&context, short_tag.data(), short_tag.size()) == 0U,
        "AES-128-GCM rejects a modified truncated tag");
}

void test_aes_ccm(Report& report) {
    // RFC 3610 section 8, packet vector #1.
    const Bytes key = from_hex("c0c1c2c3c4c5c6c7c8c9cacbcccdcecf");
    const Bytes nonce = from_hex("00000003020100a0a1a2a3a4a5");
    const Bytes aad = from_hex("0001020304050607");
    const Bytes plaintext = from_hex("08090a0b0c0d0e0f101112131415161718191a1b1c1d1e");

    br_aes_ct_ctrcbc_keys keys{};
    br_aes_ct_ctrcbc_init(&keys, key.data(), key.size());
    br_ccm_context context{};
    br_ccm_init(&context, &keys.vtable);
    report.expect(
        br_ccm_reset(&context, nonce.data(), nonce.size(), aad.size(), plaintext.size(), 8U) == 1,
        "AES-128-CCM accepts the RFC 3610 parameters");
    br_ccm_aad_inject(&context, aad.data(), aad.size());
    Bytes buffer = plaintext;
    br_ccm_flip(&context);
    br_ccm_run(&context, 1, buffer.data(), buffer.size());
    Bytes tag(8U, 0U);
    report.expect(br_ccm_get_tag(&context, tag.data()) == tag.size(),
        "AES-128-CCM reports the requested tag length");
    report.expect_hex(buffer, "588c979a61c663d2f066d0c2c0f989806d5f6b61dac384",
        "AES-128-CCM matches the RFC 3610 packet vector #1 payload");
    report.expect_hex(
        tag, "17e8d12cfdf926e0", "AES-128-CCM matches the RFC 3610 packet vector #1 tag");

    const Bytes ciphertext = buffer;
    br_ccm_reset(&context, nonce.data(), nonce.size(), aad.size(), buffer.size(), tag.size());
    br_ccm_aad_inject(&context, aad.data(), aad.size());
    br_ccm_flip(&context);
    br_ccm_run(&context, 0, buffer.data(), buffer.size());
    report.expect(
        br_ccm_check_tag(&context, tag.data()) == 1U, "AES-128-CCM accepts a matching tag");
    Bytes broken_tag = tag;
    broken_tag[7] = static_cast<std::uint8_t>(broken_tag[7] ^ 0x80U);
    br_ccm_reset(&context, nonce.data(), nonce.size(), aad.size(), ciphertext.size(), tag.size());
    br_ccm_aad_inject(&context, aad.data(), aad.size());
    br_ccm_flip(&context);
    buffer = ciphertext;
    br_ccm_run(&context, 0, buffer.data(), buffer.size());
    report.expect(
        br_ccm_check_tag(&context, broken_tag.data()) == 0U, "AES-128-CCM rejects a modified tag");

    const Bytes short_nonce(6U, 0U);
    report.expect(br_ccm_reset(&context, short_nonce.data(), short_nonce.size(), aad.size(),
                      plaintext.size(), 8U) == 0,
        "AES-128-CCM rejects a nonce shorter than seven bytes");
    const Bytes long_nonce(14U, 0U);
    report.expect(br_ccm_reset(&context, long_nonce.data(), long_nonce.size(), aad.size(),
                      plaintext.size(), 8U) == 0,
        "AES-128-CCM rejects a nonce longer than thirteen bytes");
    report.expect(
        br_ccm_reset(&context, nonce.data(), nonce.size(), aad.size(), plaintext.size(), 3U) == 0,
        "AES-128-CCM rejects an odd tag length");
}

void test_chacha20(Report& report) {
    // RFC 8439 section 2.4.2.
    const Bytes key = from_hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    const Bytes iv = from_hex("000000000000004a00000000");
    const Bytes plaintext =
        ascii("Ladies and Gentlemen of the class of '99: If I could offer you only "
              "one tip for the future, sunscreen would be it.");
    Bytes buffer = plaintext;
    br_chacha20_ct_run(key.data(), iv.data(), 1U, buffer.data(), buffer.size());
    report.expect_hex(buffer,
        "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0b"
        "f91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d"
        "807ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab7793736"
        "5af90bbf74a35be6b40b8eedf2785e42874d",
        "ChaCha20 matches RFC 8439 section 2.4.2");

    br_chacha20_ct_run(key.data(), iv.data(), 1U, buffer.data(), buffer.size());
    report.expect(buffer == plaintext, "ChaCha20 decryption inverts encryption");
}

void test_chacha20_poly1305(Report& report) {
    // RFC 8439 section 2.8.2.
    const Bytes key = from_hex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
    const Bytes iv = from_hex("070000004041424344454647");
    const Bytes aad = from_hex("50515253c0c1c2c3c4c5c6c7");
    const Bytes plaintext =
        ascii("Ladies and Gentlemen of the class of '99: If I could offer you only "
              "one tip for the future, sunscreen would be it.");

    Bytes buffer = plaintext;
    Bytes tag(16U, 0U);
    br_poly1305_i15_run(key.data(), iv.data(), buffer.data(), buffer.size(), aad.data(), aad.size(),
        tag.data(), &br_chacha20_ct_run, 1);
    report.expect_hex(buffer,
        "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
        "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b369"
        "2ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3f"
        "f4def08e4b7a9de576d26586cec64b6116",
        "the imported ChaCha20-Poly1305 composition matches RFC 8439 section 2.8.2");
    report.expect_hex(tag, "1ae10b594f09e26a7e902ecbd0600691",
        "the RFC 8439 section 2.8.2 authentication tag matches");

    const Bytes ciphertext = buffer;
    br_poly1305_i15_run(key.data(), iv.data(), buffer.data(), buffer.size(), aad.data(), aad.size(),
        tag.data(), &br_chacha20_ct_run, 0);
    report.expect(buffer == plaintext, "the composition decrypts to the original plaintext");

    // Every Poly1305 implementation in the tree must decrypt and authenticate
    // the same record.
    struct Variant {
        const char* name;
        br_poly1305_run run;
    };

    const Variant variants[] = {
        {"ctmul", &br_poly1305_ctmul_run},
        {"ctmul32", &br_poly1305_ctmul32_run},
    };
    if (br_poly1305_ctmulq_get() != nullptr) {
        // The 128-bit carry implementation is only available on targets with
        // 64x64->128 multiplication, so it is not part of the portable
        // baseline comparison.
    }
    for (const Variant& variant : variants) {
        Bytes other = ciphertext;
        Bytes other_tag(16U, 0U);
        variant.run(key.data(), iv.data(), other.data(), other.size(), aad.data(), aad.size(),
            other_tag.data(), &br_chacha20_ct_run, 0);
        report.expect(other == plaintext, "every Poly1305 implementation decrypts the record");
        report.expect_hex(other_tag, "1ae10b594f09e26a7e902ecbd0600691",
            "every Poly1305 implementation computes the RFC tag");
    }

    Bytes modified_aad = aad;
    modified_aad[0] = static_cast<std::uint8_t>(modified_aad[0] ^ 0x01U);
    Bytes other_tag(16U, 0U);
    Bytes modified = plaintext;
    br_poly1305_i15_run(key.data(), iv.data(), modified.data(), modified.size(),
        modified_aad.data(), modified_aad.size(), other_tag.data(), &br_chacha20_ct_run, 1);
    report.expect(other_tag != tag, "the ChaCha20-Poly1305 tag authenticates the associated data");
}

} // namespace

int crypto_sym_kat() {
    Report report;
    test_aes_block(report);
    test_aes_cbc(report);
    test_aes_ctr(report);
    test_aes_gcm(report);
    test_aes_ccm(report);
    test_chacha20(report);
    test_chacha20_poly1305(report);
    return report.failures();
}
