/*
  crypto_hash_kat.cpp - host known-answer tests for the imported BearSSL hashes

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Vector sources:
    MD5        RFC 1321 Appendix A.5 test suite
    SHA-1      FIPS 180-1 / RFC 3174 examples
    SHA-2      FIPS 180-2 examples for "abc", the two-block string, and
               one million "a" bytes
    HMAC       RFC 4231 test cases 1, 2, 6, and 7
    HKDF       RFC 5869 Appendix A.1 and A.3
*/

#include "crypto_kat.h"

#include "bearssl.h"

#include <array>

namespace {

using crypto_kat::Bytes;
using crypto_kat::from_hex;
using crypto_kat::Report;

Bytes ascii(const char* const text) {
    return Bytes(text, text + std::strlen(text));
}

/** @brief Feeds one message to SHA-256 in uneven chunks. */
Bytes chunked_sha256(const Bytes& message, const std::size_t step) {
    br_sha256_context context{};
    br_sha256_init(&context);
    std::size_t offset = 0;
    while (offset < message.size()) {
        const auto remaining = message.size() - offset;
        const auto chunk = remaining < step ? remaining : step;
        br_sha256_update(&context, message.data() + offset, chunk);
        offset += chunk;
    }
    Bytes digest(32U, 0U);
    br_sha256_out(&context, digest.data());
    return digest;
}

void test_md5(Report& report) {
    struct Vector {
        const char* input;
        const char* digest;
    };

    // RFC 1321 Appendix A.5.
    const Vector vectors[] = {
        {"", "d41d8cd98f00b204e9800998ecf8427e"},
        {"a", "0cc175b9c0f1b6a831c399e269772661"},
        {"abc", "900150983cd24fb0d6963f7d28e17f72"},
        {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
        {"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
        {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
            "d174ab98d277d9f5a5611c2c9f419d9f"},
        {"12345678901234567890123456789012345678901234567890123456789012345678901234567890",
            "57edf4a22be3c955ac49da2e2107b67a"},
    };
    for (const Vector& vector : vectors) {
        br_md5_context context{};
        br_md5_init(&context);
        br_md5_update(&context, vector.input, std::strlen(vector.input));
        Bytes digest(16U, 0U);
        br_md5_out(&context, digest.data());
        report.expect_hex(digest, vector.digest, "MD5 matches the RFC 1321 test suite entry");
    }
}

void test_sha1(Report& report) {
    br_sha1_context context{};
    br_sha1_init(&context);
    br_sha1_update(&context, "abc", 3U);
    Bytes digest(20U, 0U);
    br_sha1_out(&context, digest.data());
    report.expect_hex(
        digest, "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA-1 of \"abc\" matches FIPS 180-1");

    br_sha1_init(&context);
    br_sha1_update(&context, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56U);
    br_sha1_out(&context, digest.data());
    report.expect_hex(digest, "84983e441c3bd26ebaae4aa1f95129e5e54670f1",
        "SHA-1 of the two-block example matches FIPS 180-1");

    const Bytes thousand(1000U, static_cast<std::uint8_t>('a'));
    br_sha1_init(&context);
    for (int round = 0; round < 1000; ++round) {
        br_sha1_update(&context, thousand.data(), thousand.size());
    }
    br_sha1_out(&context, digest.data());
    report.expect_hex(digest, "34aa973cd4c4daa4f61eeb2bdbad27316534016f",
        "SHA-1 of one million bytes matches FIPS 180-1");
}

void test_sha2(Report& report) {
    br_sha224_context sha224{};
    br_sha224_init(&sha224);
    br_sha224_update(&sha224, "abc", 3U);
    Bytes digest224(28U, 0U);
    br_sha224_out(&sha224, digest224.data());
    report.expect_hex(digest224, "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7",
        "SHA-224 of \"abc\" matches FIPS 180-2");

    br_sha256_context sha256{};
    br_sha256_init(&sha256);
    br_sha256_update(&sha256, "abc", 3U);
    Bytes sha256_digest(32U, 0U);
    br_sha256_out(&sha256, sha256_digest.data());
    report.expect_hex(sha256_digest,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "SHA-256 of \"abc\" matches FIPS 180-2");

    br_sha256_init(&sha256);
    br_sha256_update(&sha256, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56U);
    br_sha256_out(&sha256, sha256_digest.data());
    report.expect_hex(sha256_digest,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
        "SHA-256 of the two-block example matches FIPS 180-2");

    const Bytes thousand(1000U, static_cast<std::uint8_t>('a'));
    br_sha256_init(&sha256);
    for (int round = 0; round < 1000; ++round) {
        br_sha256_update(&sha256, thousand.data(), thousand.size());
    }
    br_sha256_out(&sha256, sha256_digest.data());
    report.expect_hex(sha256_digest,
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
        "SHA-256 of one million bytes matches FIPS 180-2");

    br_sha384_context sha384{};
    br_sha384_init(&sha384);
    br_sha384_update(&sha384, "abc", 3U);
    Bytes sha384_digest(48U, 0U);
    br_sha384_out(&sha384, sha384_digest.data());
    report.expect_hex(sha384_digest,
        "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
        "8086072ba1e7cc2358baeca134c825a7",
        "SHA-384 of \"abc\" matches FIPS 180-2");

    br_sha512_context sha512{};
    br_sha512_init(&sha512);
    br_sha512_update(&sha512, "abc", 3U);
    Bytes sha512_digest(64U, 0U);
    br_sha512_out(&sha512, sha512_digest.data());
    report.expect_hex(sha512_digest,
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
        "SHA-512 of \"abc\" matches FIPS 180-2");

    br_sha512_init(&sha512);
    br_sha512_update(&sha512, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56U);
    br_sha512_out(&sha512, sha512_digest.data());
    report.expect_hex(sha512_digest,
        "204a8fc6dda82f0a0ced7beb8e08a41657c16ef468b228a8279be331a703c33596fd15c13b"
        "1b07f9aa1d3bea57789ca031ad85c7a71dd70354ec631238ca3445",
        "SHA-512 of the two-block example matches FIPS 180-2");
}

void test_chunked_hashing(Report& report) {
    const Bytes message =
        ascii("The quick brown fox jumps over the lazy dog, repeatedly and in several "
              "chunks, so that block boundaries and partial blocks are exercised.");
    for (const std::size_t step : {1U, 7U, 31U, 63U, 64U, 65U, 127U, 128U}) {
        const Bytes single = chunked_sha256(message, message.size());
        const Bytes chunked = chunked_sha256(message, step);
        report.expect(single == chunked, "SHA-256 is independent of the update chunk size");
    }
    const Bytes empty_chunked = chunked_sha256(Bytes{}, 8U);
    report.expect_hex(empty_chunked,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "SHA-256 of an empty message matches FIPS 180-2");
}

void test_hash_state(Report& report) {
    // The imported running-state blob holds only the chaining values, so a
    // capture has to happen on a block boundary.
    const Bytes first = Bytes(128U, static_cast<std::uint8_t>('a'));
    const Bytes second = ascii("second chunk of the hashed message");

    br_sha256_context whole{};
    br_sha256_init(&whole);
    br_sha256_update(&whole, first.data(), first.size());
    br_sha256_update(&whole, second.data(), second.size());
    Bytes whole_digest(32U, 0U);
    br_sha256_out(&whole, whole_digest.data());

    br_sha256_context captured{};
    br_sha256_init(&captured);
    br_sha256_update(&captured, first.data(), first.size());
    std::array<std::uint8_t, 32U> state{};
    const std::uint64_t count = br_sha256_state(&captured, state.data());
    report.expect(count == first.size(), "SHA-256 state reports the injected byte count");
    br_sha256_update(&captured, second.data(), second.size());
    Bytes continued(32U, 0U);
    br_sha256_out(&captured, continued.data());
    report.expect(continued == whole_digest, "SHA-256 state capture does not change the digest");

    br_sha256_context resumed{};
    br_sha256_init(&resumed);
    br_sha256_set_state(&resumed, state.data(), count);
    br_sha256_update(&resumed, second.data(), second.size());
    Bytes restored(32U, 0U);
    br_sha256_out(&resumed, restored.data());
    report.expect(
        restored == whole_digest, "SHA-256 restored state continues the original message");

    br_sha512_context whole512{};
    br_sha512_init(&whole512);
    br_sha512_update(&whole512, first.data(), first.size());
    br_sha512_update(&whole512, second.data(), second.size());
    Bytes whole512_digest(64U, 0U);
    br_sha512_out(&whole512, whole512_digest.data());

    br_sha512_context captured512{};
    br_sha512_init(&captured512);
    br_sha512_update(&captured512, first.data(), first.size());
    std::array<std::uint8_t, 64U> state512{};
    const std::uint64_t count512 = br_sha512_state(&captured512, state512.data());
    br_sha512_init(&captured512);
    br_sha512_set_state(&captured512, state512.data(), count512);
    br_sha512_update(&captured512, second.data(), second.size());
    Bytes restored512(64U, 0U);
    br_sha512_out(&captured512, restored512.data());
    report.expect(count512 == first.size() && restored512 == whole512_digest,
        "SHA-512 state capture and restore continue the original message");
}

void test_multihash(Report& report) {
    const Bytes message = ascii("multihash digest of one message");
    br_multihash_context context{};
    br_multihash_zero(&context);
    br_multihash_setimpl(&context, br_sha256_ID, &br_sha256_vtable);
    br_multihash_setimpl(&context, br_sha1_ID, &br_sha1_vtable);
    br_multihash_init(&context);
    br_multihash_update(&context, message.data(), message.size());
    Bytes sha256_digest(32U, 0U);
    report.expect(
        br_multihash_out(&context, br_sha256_ID, sha256_digest.data()) == sha256_digest.size(),
        "multihash returns the full SHA-256 output length");
    br_sha256_context direct{};
    br_sha256_init(&direct);
    br_sha256_update(&direct, message.data(), message.size());
    Bytes direct_digest(32U, 0U);
    br_sha256_out(&direct, direct_digest.data());
    report.expect(
        sha256_digest == direct_digest, "multihash SHA-256 agrees with the direct implementation");

    Bytes sha1_digest(20U, 0U);
    report.expect(br_multihash_out(&context, br_sha1_ID, sha1_digest.data()) == sha1_digest.size(),
        "multihash returns the full SHA-1 output length");
    br_sha1_context direct1{};
    br_sha1_init(&direct1);
    br_sha1_update(&direct1, message.data(), message.size());
    Bytes direct1_digest(20U, 0U);
    br_sha1_out(&direct1, direct1_digest.data());
    report.expect(
        sha1_digest == direct1_digest, "multihash SHA-1 agrees with the direct implementation");
}

/** @brief Computes one HMAC-SHA-256 value over a single message. */
Bytes hmac_sha256(const Bytes& key, const Bytes& data) {
    br_hmac_key_context key_context{};
    br_hmac_key_init(&key_context, &br_sha256_vtable, key.data(), key.size());
    br_hmac_context context{};
    br_hmac_init(&context, &key_context, 0U);
    br_hmac_update(&context, data.data(), data.size());
    Bytes mac(32U, 0U);
    br_hmac_out(&context, mac.data());
    return mac;
}

void test_hmac_sha256(Report& report) {
    // RFC 4231 test case 1.
    report.expect_hex(hmac_sha256(Bytes(20U, 0x0bU), ascii("Hi There")),
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
        "HMAC-SHA-256 matches RFC 4231 test case 1");

    // RFC 4231 test case 2.
    report.expect_hex(hmac_sha256(ascii("Jefe"), ascii("what do ya want for nothing?")),
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
        "HMAC-SHA-256 matches RFC 4231 test case 2");

    // RFC 4231 test case 6: a key longer than the hash block size.
    report.expect_hex(hmac_sha256(Bytes(131U, 0xaaU),
                          ascii("Test Using Larger Than Block-Size Key - Hash Key First")),
        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
        "HMAC-SHA-256 hashes an over-long key as RFC 4231 test case 6 requires");

    // RFC 4231 test case 7: an over-long key and over-long data.
    report.expect_hex(hmac_sha256(Bytes(131U, 0xaaU),
                          ascii("This is a test using a larger than block-size key and a "
                                "larger than block-size data. The key needs to be hashed "
                                "before being used by the HMAC algorithm.")),
        "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2",
        "HMAC-SHA-256 matches RFC 4231 test case 7");
}

void test_hkdf_sha256(Report& report) {
    const Bytes ikm(22U, 0x0bU);
    const Bytes a1_salt = from_hex("000102030405060708090a0b0c");
    const Bytes a1_info = from_hex("f0f1f2f3f4f5f6f7f8f9");

    br_hkdf_context context{};
    br_hkdf_init(&context, &br_sha256_vtable, a1_salt.data(), a1_salt.size());
    br_hkdf_inject(&context, ikm.data(), ikm.size());
    br_hkdf_flip(&context);
    Bytes okm(42U, 0U);
    std::size_t produced =
        br_hkdf_produce(&context, a1_info.data(), a1_info.size(), okm.data(), okm.size());
    report.expect(produced == okm.size(), "HKDF-Expand returns the requested output length");
    report.expect_hex(okm,
        "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b8"
        "87185865",
        "RFC 5869 A.1 HKDF-SHA-256 output keying material matches");

    // RFC 5869 A.3: absent salt and empty info.
    br_hkdf_init(&context, &br_sha256_vtable, BR_HKDF_NO_SALT, 0U);
    br_hkdf_inject(&context, ikm.data(), ikm.size());
    br_hkdf_flip(&context);
    Bytes a3_okm(42U, 0U);
    produced = br_hkdf_produce(&context, nullptr, 0U, a3_okm.data(), a3_okm.size());
    report.expect(
        produced == a3_okm.size(), "RFC 5869 A.3 HKDF-SHA-256 produces the requested length");
    report.expect_hex(a3_okm,
        "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa"
        "4b61a96c8",
        "RFC 5869 A.3 HKDF-SHA-256 output keying material matches");
}

void test_hkdf_chunked_output(Report& report) {
    const Bytes ikm(22U, 0x0bU);
    const Bytes salt = from_hex("000102030405060708090a0b0c");
    const Bytes info = from_hex("f0f1f2f3f4f5f6f7f8f9");

    br_hkdf_context context{};
    br_hkdf_init(&context, &br_sha256_vtable, salt.data(), salt.size());
    br_hkdf_inject(&context, ikm.data(), ikm.size());
    br_hkdf_flip(&context);
    Bytes chunked(42U, 0U);
    std::size_t written = 0;
    while (written < chunked.size()) {
        written += br_hkdf_produce(
            &context, info.data(), info.size(), chunked.data() + written, chunked.size() - written);
    }
    report.expect_hex(chunked,
        "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b8"
        "87185865",
        "chunked HKDF-SHA-256 output matches the single-call output");

    // The first two HKDF blocks already exceed one hash output; longer output
    // continues with successive block counters up to the RFC limit.
    Bytes long_output(64U, 0U);
    br_hkdf_init(&context, &br_sha256_vtable, salt.data(), salt.size());
    br_hkdf_inject(&context, ikm.data(), ikm.size());
    br_hkdf_flip(&context);
    const std::size_t produced =
        br_hkdf_produce(&context, info.data(), info.size(), long_output.data(), long_output.size());
    report.expect(produced == long_output.size(), "HKDF-SHA-256 produces two full hash blocks");
    Bytes split_output(32U, 0U);
    Bytes second_half(32U, 0U);
    br_hkdf_init(&context, &br_sha256_vtable, salt.data(), salt.size());
    br_hkdf_inject(&context, ikm.data(), ikm.size());
    br_hkdf_flip(&context);
    br_hkdf_produce(&context, info.data(), info.size(), split_output.data(), split_output.size());
    br_hkdf_produce(&context, info.data(), info.size(), second_half.data(), second_half.size());
    report.expect(
        long_output.size() == split_output.size() + second_half.size() &&
            std::memcmp(long_output.data(), split_output.data(), split_output.size()) == 0 &&
            std::memcmp(long_output.data() + split_output.size(), second_half.data(),
                second_half.size()) == 0,
        "separate HKDF-SHA-256 calls return consecutive blocks");
}

} // namespace

int crypto_hash_kat() {
    Report report;
    test_md5(report);
    test_sha1(report);
    test_sha2(report);
    test_chunked_hashing(report);
    test_hash_state(report);
    test_multihash(report);
    test_hmac_sha256(report);
    test_hkdf_sha256(report);
    test_hkdf_chunked_output(report);
    return report.failures();
}
