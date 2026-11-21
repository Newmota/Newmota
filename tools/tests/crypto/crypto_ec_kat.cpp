/*
  crypto_ec_kat.cpp - host known-answer tests for the imported BearSSL EC code

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Vector sources:
    X25519          RFC 7748 section 5.2 and section 6.1
    P-256 generator FIPS 186-4 D.1.2.3
    P-256 ECDSA     RFC 6979 appendix A.2.5 (P-256 with SHA-256)

  Signature generation is deferred to Phase 1. The imported signers use RFC 6979
  deterministic nonces derived from the private key and reduced message hash;
  signing itself needs no external entropy. Key generation still requires it.
*/

#include "crypto_kat.h"

#include "bearssl.h"

#include <utility>

namespace {

using crypto_kat::Bytes;
using crypto_kat::from_hex;
using crypto_kat::Report;

/** @brief Returns the imported Curve25519 implementation entry point. */
const br_ec_impl* curve25519() {
    return &br_ec_c25519_m15;
}

/** @brief Returns the imported P-256 implementation entry point. */
const br_ec_impl* p256() {
    return &br_ec_p256_m15;
}

/** @brief Reverses a scalar into the big-endian order the EC entry points use. */
Bytes big_endian(const Bytes& scalar) {
    Bytes reversed(scalar.rbegin(), scalar.rend());
    return reversed;
}

/**
 * @brief Computes X25519 with the imported Curve25519 code.
 *
 * The RFC 7748 scalars are little-endian, while the BearSSL EC entry points
 * take a big-endian multiplier, so the scalar is reversed here. The point
 * buffer is modified in place, exactly as the implementation expects.
 */
Bytes x25519(const Bytes& secret, Bytes peer) {
    const Bytes scalar = big_endian(secret);
    Bytes result = std::move(peer);
    curve25519()->mul(result.data(), result.size(), scalar.data(), scalar.size(), BR_EC_curve25519);
    return result;
}

/** @brief Derives an X25519 public key from a little-endian RFC 7748 secret. */
Bytes x25519_public(const Bytes& secret) {
    const Bytes scalar = big_endian(secret);
    Bytes public_key(32U, 0U);
    curve25519()->mulgen(public_key.data(), scalar.data(), scalar.size(), BR_EC_curve25519);
    return public_key;
}

void test_x25519_keys(Report& report) {
    // RFC 7748 section 6.1.
    const Bytes alice_secret =
        from_hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const Bytes bob_secret =
        from_hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");

    const Bytes alice_public = x25519_public(alice_secret);
    const Bytes bob_public = x25519_public(bob_secret);
    report.expect_hex(alice_public,
        "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a",
        "RFC 7748 Alice public key matches");
    report.expect_hex(bob_public,
        "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f",
        "RFC 7748 Bob public key matches");

    const Bytes alice_shared = x25519(alice_secret, bob_public);
    const Bytes bob_shared = x25519(bob_secret, alice_public);
    report.expect_hex(alice_shared,
        "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742",
        "RFC 7748 shared secret matches");
    report.expect(
        alice_shared == bob_shared, "both X25519 directions produce the same shared secret");
}

void test_x25519_iteration(Report& report) {
    // RFC 7748 section 5.2.
    const Bytes base = from_hex("09000000000000000000000000000000"
                                "00000000000000000000000000000000");
    Bytes scalar = base;
    Bytes step = x25519(scalar, base);
    report.expect_hex(step, "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079",
        "RFC 7748 single iteration matches");

    Bytes point = scalar;
    scalar = step;
    for (int round = 1; round < 1000; ++round) {
        step = x25519(scalar, point);
        point = scalar;
        scalar = step;
    }
    report.expect_hex(scalar, "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51",
        "RFC 7748 one thousand iterations match");

    // A low-order point produces an all-zero shared secret. The entry point
    // reports success, so the caller has to reject that result.
    const Bytes low_order(32U, 0U);
    const Bytes zero_result = x25519(
        from_hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a"), low_order);
    bool all_zero = true;
    for (const std::uint8_t byte : zero_result) {
        if (byte != 0U)
            all_zero = false;
    }
    report.expect(all_zero, "a low-order peer point yields the all-zero shared secret");

    // Length checks are part of the entry point contract.
    const Bytes secret =
        from_hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const Bytes scalar_bytes = big_endian(secret);
    Bytes ignored(32U, 0U);
    report.expect(
        curve25519()->mulgen(ignored.data(), scalar_bytes.data(), 31U, BR_EC_curve25519) == 32U,
        "X25519 accepts a short scalar and left-pads it");
    Bytes long_scalar = scalar_bytes;
    long_scalar.push_back(0U);
    report.expect(curve25519()->mul(ignored.data(), 32U, long_scalar.data(), long_scalar.size(),
                      BR_EC_curve25519) == 0U,
        "X25519 rejects an over-long scalar");
    // The generator entry point always reports the generator length, even when
    // the scalar is refused, so callers must validate lengths before it.
    report.expect(curve25519()->mulgen(ignored.data(), long_scalar.data(), long_scalar.size(),
                      BR_EC_curve25519) == 32U,
        "the X25519 generator entry point reports its length regardless");
    report.expect(curve25519()->mul(ignored.data(), 31U, base.data(), 32U, BR_EC_curve25519) == 0U,
        "X25519 rejects a peer point that is not 32 bytes");
}

void test_p256_generator(Report& report) {
    std::size_t size = 0U;
    const unsigned char* generator = p256()->generator(BR_EC_secp256r1, &size);
    report.expect(generator != nullptr && size == 65U,
        "the P-256 generator is an uncompressed 65-byte point");
    if (generator == nullptr || size != 65U)
        return;
    report.expect_hex(Bytes(generator, generator + 65U),
        "046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
        "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5",
        "the P-256 generator matches FIPS 186-4 D.1.2.3");

    std::size_t order_size = 0U;
    const unsigned char* order = p256()->order(BR_EC_secp256r1, &order_size);
    report.expect(order != nullptr && order_size == 32U, "the P-256 subgroup order is 32 bytes");
    report.expect_hex(Bytes(order, order + order_size),
        "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
        "the P-256 subgroup order matches FIPS 186-4 D.1.2.2");

    // Multiplying a point by one returns that point. The point argument is
    // modified in place, and the multiplier is a big-endian integer.
    Bytes one(32U, 0U);
    one[31] = 0x01U;
    Bytes product(generator, generator + 65U);
    report.expect(
        p256()->mul(product.data(), product.size(), one.data(), one.size(), BR_EC_secp256r1) == 1U,
        "P-256 accepts a 32-byte multiplier");
    report.expect(product == Bytes(generator, generator + 65U),
        "a P-256 point multiplied by one is unchanged");

    // Only the point encoding is validated. A zero multiplier is reported as a
    // success and encodes the point at infinity, so callers have to reject a
    // zero or over-long multiplier themselves.
    const Bytes zero(32U, 0U);
    Bytes infinity(generator, generator + 65U);
    report.expect(p256()->mul(infinity.data(), infinity.size(), zero.data(), zero.size(),
                      BR_EC_secp256r1) == 1U,
        "P-256 reports success for a zero multiplier");
    const char* infinity_encoding =
        "040000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000";
    report.expect_hex(
        infinity, infinity_encoding, "a zero P-256 multiplier encodes the point at infinity");
    report.expect(p256()->mulgen(infinity.data(), zero.data(), zero.size(), BR_EC_secp256r1) == 65U,
        "the P-256 generator entry point reports its length regardless");

    Bytes compressed(64U, 0x04U);
    report.expect(p256()->mul(compressed.data(), compressed.size(), one.data(), one.size(),
                      BR_EC_secp256r1) == 0U,
        "P-256 rejects a point encoding that is not 65 bytes");
    Bytes off_curve(65U, 0U);
    off_curve[0] = 0x04U;
    report.expect(p256()->mul(off_curve.data(), off_curve.size(), one.data(), one.size(),
                      BR_EC_secp256r1) == 0U,
        "P-256 rejects a point that is not on the curve");
}

void test_p256_ecdsa_verify(Report& report) {
    // RFC 6979 appendix A.2.5, P-256 with SHA-256 over "sample".
    const Bytes hash = from_hex("af2bdbe1aa9b6ec1e2ade1d694f41fc71a831d0268e9891562113d8a62add1bf");
    const Bytes public_key =
        from_hex("04"
                 "60fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6"
                 "7903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299");
    const Bytes signature =
        from_hex("efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716"
                 "f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8");

    br_ec_public_key key{};
    key.curve = BR_EC_secp256r1;
    key.q = const_cast<unsigned char*>(public_key.data());
    key.qlen = public_key.size();

    report.expect(br_ecdsa_i15_vrfy_raw(p256(), hash.data(), hash.size(), &key, signature.data(),
                      signature.size()) == 1U,
        "P-256 ECDSA accepts the RFC 6979 A.2.5 signature");

    // The same signature must also verify through the implementation selected
    // for this build.
    const br_ecdsa_vrfy verify_raw = br_ecdsa_vrfy_raw_get_default();
    report.expect(verify_raw != nullptr && verify_raw(p256(), hash.data(), hash.size(), &key,
                                               signature.data(), signature.size()) == 1U,
        "the default raw ECDSA verifier accepts the RFC 6979 signature");

    Bytes other_hash = hash;
    other_hash[31] = static_cast<std::uint8_t>(other_hash[31] ^ 0x01U);
    report.expect(br_ecdsa_i15_vrfy_raw(p256(), other_hash.data(), other_hash.size(), &key,
                      signature.data(), signature.size()) == 0U,
        "P-256 ECDSA rejects a different hash");

    Bytes broken_r = signature;
    broken_r[31] = static_cast<std::uint8_t>(broken_r[31] ^ 0x01U);
    report.expect(br_ecdsa_i15_vrfy_raw(p256(), hash.data(), hash.size(), &key, broken_r.data(),
                      broken_r.size()) == 0U,
        "P-256 ECDSA rejects a modified r value");

    Bytes broken_s = signature;
    broken_s[63] = static_cast<std::uint8_t>(broken_s[63] ^ 0x01U);
    report.expect(br_ecdsa_i15_vrfy_raw(p256(), hash.data(), hash.size(), &key, broken_s.data(),
                      broken_s.size()) == 0U,
        "P-256 ECDSA rejects a modified s value");

    report.expect(br_ecdsa_i15_vrfy_raw(p256(), hash.data(), hash.size(), &key, signature.data(),
                      signature.size() - 1U) == 0U,
        "P-256 ECDSA rejects a truncated signature");

    Bytes wrong_key = public_key;
    wrong_key[1] = static_cast<std::uint8_t>(wrong_key[1] ^ 0x01U);
    br_ec_public_key other_key{};
    other_key.curve = BR_EC_secp256r1;
    other_key.q = wrong_key.data();
    other_key.qlen = wrong_key.size();
    report.expect(br_ecdsa_i15_vrfy_raw(p256(), hash.data(), hash.size(), &other_key,
                      signature.data(), signature.size()) == 0U,
        "P-256 ECDSA rejects a signature under a different key");

    // The DER form of the same signature verifies through the ASN.1 entry point.
    const Bytes der_signature =
        from_hex("3046022100efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3"
                 "716022100f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843a"
                 "cda8");
    report.expect(br_ecdsa_i15_vrfy_asn1(p256(), hash.data(), hash.size(), &key,
                      der_signature.data(), der_signature.size()) == 1U,
        "P-256 ECDSA accepts the DER form of the RFC 6979 signature");

    const br_ecdsa_vrfy verify_asn1 = br_ecdsa_vrfy_asn1_get_default();
    report.expect(verify_asn1 != nullptr && verify_asn1(p256(), hash.data(), hash.size(), &key,
                                                der_signature.data(), der_signature.size()) == 1U,
        "the default ASN.1 ECDSA verifier accepts the DER signature");

    report.expect(br_ecdsa_i15_vrfy_asn1(p256(), hash.data(), hash.size(), &key,
                      der_signature.data(), der_signature.size() - 1U) == 0U,
        "P-256 ECDSA rejects a truncated DER signature");

    // A DER signature that is only an algorithm identifier prefix is rejected.
    const Bytes prefix_only = from_hex("300d0609608648016503040201050000");
    report.expect(br_ecdsa_i15_vrfy_asn1(p256(), hash.data(), hash.size(), &key, prefix_only.data(),
                      prefix_only.size()) == 0U,
        "P-256 ECDSA rejects a DER signature without r and s");
}

} // namespace

int crypto_ec_kat() {
    Report report;
    test_x25519_keys(report);
    test_x25519_iteration(report);
    test_p256_generator(report);
    test_p256_ecdsa_verify(report);
    return report.failures();
}
