/*
  web_tls_identity.cpp - device-persistent self-signed HTTPS identity

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota_features.h"
#include "services/web/web_tls_identity.h"

#include "core/storage/filesystem.h"
#include "newmota/platform/network.h"
#include "newmota/platform/web.h"
#include "support/crc32.h"

#include "mbedtls/version.h"
#if MBEDTLS_VERSION_MAJOR >= 3
#include "mbedtls/build_info.h"
#else
#include "mbedtls/config.h"
#endif
#if MBEDTLS_VERSION_MAJOR < 4
#include "mbedtls/ctr_drbg.h"
#endif
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"

#if MBEDTLS_VERSION_MAJOR >= 4
#include "psa/crypto.h"
#else
#include "mbedtls/ecp.h"
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace newmota {
namespace {

constexpr std::string_view kIdentityPath{"/.newmota/web-identity.bin"};
constexpr std::string_view kTemporaryPath{"/.newmota/web-identity.tmp"};
constexpr std::string_view kDirectory{"/.newmota"};
constexpr std::size_t kHeaderSize = 10U;
constexpr std::size_t kChecksumSize = 4U;
constexpr std::uint8_t kIdentityVersion = 1U;

platform::WebTlsIdentity identity{};
bool identity_ready{};

enum class IdentityReadResult : std::uint8_t { succeeded, not_found, corrupt, io_error };

/** @brief Reads a complete chunk, distinguishing truncation from an I/O failure. */
IdentityReadResult read_exact(
    filesystem::FileReader& reader, void* destination, const std::size_t size) {
    auto* output = static_cast<std::uint8_t*>(destination);
    std::size_t offset = 0U;
    while (offset < size) {
        std::size_t count = 0U;
        if (filesystem::read(reader, output + offset, size - offset, count) !=
            filesystem::ReadResult::succeeded)
            return IdentityReadResult::io_error;
        if (count == 0U)
            return IdentityReadResult::corrupt;
        offset += count;
    }
    return IdentityReadResult::succeeded;
}

/** @brief Encodes a 16-bit integer in the identity record's stable byte order. */
void write_u16(std::uint8_t* output, const std::uint16_t value) {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8U);
}

/** @brief Decodes a 16-bit integer from the identity record. */
std::uint16_t read_u16(const std::uint8_t* input) {
    return static_cast<std::uint16_t>(input[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(input[1]) << 8U);
}

/** @brief Encodes a 32-bit integer in the identity record's stable byte order. */
void write_u32(std::uint8_t* output, const std::uint32_t value) {
    for (std::size_t index = 0U; index < 4U; ++index)
        output[index] = static_cast<std::uint8_t>(value >> (index * 8U));
}

/** @brief Decodes a 32-bit integer from the identity record. */
std::uint32_t read_u32(const std::uint8_t* input) {
    std::uint32_t value = 0U;
    for (std::size_t index = 0U; index < 4U; ++index)
        value |= static_cast<std::uint32_t>(input[index]) << (index * 8U);
    return value;
}

/** @brief Fills Mbed TLS DRBG bytes from the selected platform's hardware RNG. */
#if MBEDTLS_VERSION_MAJOR < 4
int entropy(void*, unsigned char* output, const std::size_t size) {
    return platform::web_random(output, size) ? 0 : -1;
}
#else
bool initialize_psa() {
    const auto result = psa_crypto_init();
    return result == PSA_SUCCESS || result == PSA_ERROR_BAD_STATE;
}
#endif

/** @brief Adds one DNS GeneralName to the bounded DER SubjectAltName extension. */
bool append_dns_name(
    std::array<unsigned char, 160>& extension, std::size_t& size, const std::string_view name) {
    if (name.empty() || name.size() > 63U || size + name.size() + 2U > extension.size())
        return false;
    extension[size++] = 0x82U; // [2] dNSName, primitive context-specific IA5String.
    extension[size++] = static_cast<unsigned char>(name.size());
    for (const char character : name) {
        const auto value = static_cast<unsigned char>(character);
        const bool valid = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                           (value >= '0' && value <= '9') || value == '-' || value == '.';
        if (!valid)
            return false;
        extension[size++] = value;
    }
    return true;
}

/** @brief Parses one dotted IPv4 address and appends its IP GeneralName. */
bool append_ipv4_name(
    std::array<unsigned char, 160>& extension, std::size_t& size, const std::string_view address) {
    if (address.empty() || address == "0.0.0.0" || size + 6U > extension.size())
        return true;
    std::array<unsigned char, 4> octets{};
    std::size_t octet = 0U;
    unsigned int value = 0U;
    unsigned int digits = 0U;
    for (std::size_t index = 0U; index <= address.size(); ++index) {
        const char character = index == address.size() ? '.' : address[index];
        if (character == '.') {
            if (digits == 0U || octet >= octets.size() || value > 255U)
                return true;
            octets[octet++] = static_cast<unsigned char>(value);
            value = 0U;
            digits = 0U;
            continue;
        }
        if (character < '0' || character > '9' || digits == 3U)
            return true;
        value = value * 10U + static_cast<unsigned int>(character - '0');
        ++digits;
    }
    if (octet != octets.size())
        return true;
    extension[size++] = 0x87U; // [7] iPAddress, primitive context-specific OCTET STRING.
    extension[size++] = static_cast<unsigned char>(octets.size());
    for (const auto value_octet : octets)
        extension[size++] = value_octet;
    return true;
}

/** @brief Builds the small DER SEQUENCE used for the self-signed certificate's SAN. */
bool subject_alt_name(std::array<unsigned char, 160>& extension, std::size_t& size) {
    std::array<unsigned char, 160> entries{};
    std::size_t entry_size = 0U;
    const auto station = platform::network_info();
    const auto access_point = platform::access_point_info();
    std::string_view hostname{station.hostname};
    if (hostname.empty())
        hostname = access_point.hostname;
    if (hostname.empty())
        hostname = "newmota";
    if (!append_dns_name(entries, entry_size, hostname))
        return false;
    const bool has_local_suffix =
        hostname.size() >= 6U && hostname.substr(hostname.size() - 6U) == ".local";
    if (hostname.size() <= 57U && !has_local_suffix) {
        std::array<char, 64> local_name{};
        std::memcpy(local_name.data(), hostname.data(), hostname.size());
        std::memcpy(local_name.data() + hostname.size(), ".local", 7U);
        if (!append_dns_name(
                entries, entry_size, std::string_view{local_name.data(), hostname.size() + 6U}))
            return false;
    }
    if (!append_ipv4_name(entries, entry_size, station.address) ||
        !append_ipv4_name(entries, entry_size, access_point.address) || entry_size >= 128U)
        return false;
    extension[0] = 0x30U;
    extension[1] = static_cast<unsigned char>(entry_size);
    std::memcpy(extension.data() + 2U, entries.data(), entry_size);
    size = entry_size + 2U;
    return true;
}

/** @brief Validates that the stored certificate parses and matches its private key. */
bool validate_identity(const platform::WebTlsIdentity& candidate
#if MBEDTLS_VERSION_MAJOR < 4
    ,
    mbedtls_ctr_drbg_context* random
#endif
) {
#if MBEDTLS_VERSION_MAJOR >= 4
    if (!initialize_psa())
        return false;
#endif
    mbedtls_x509_crt certificate;
    mbedtls_pk_context private_key;
    mbedtls_x509_crt_init(&certificate);
    mbedtls_pk_init(&private_key);
    const auto cert_result = mbedtls_x509_crt_parse(&certificate,
        reinterpret_cast<const unsigned char*>(candidate.certificate_pem.data()),
        candidate.certificate_size + 1U);
#if MBEDTLS_VERSION_MAJOR >= 4
    const auto key_result = mbedtls_pk_parse_key(&private_key,
        reinterpret_cast<const unsigned char*>(candidate.private_key_pem.data()),
        candidate.private_key_size + 1U, nullptr, 0U);
    const auto pair_result = cert_result == 0 && key_result == 0
                                 ? mbedtls_pk_check_pair(&certificate.pk, &private_key)
                                 : -1;
#elif MBEDTLS_VERSION_MAJOR >= 3
    const auto key_result = mbedtls_pk_parse_key(&private_key,
        reinterpret_cast<const unsigned char*>(candidate.private_key_pem.data()),
        candidate.private_key_size + 1U, nullptr, 0U, mbedtls_ctr_drbg_random, random);
    const auto pair_result =
        cert_result == 0 && key_result == 0
            ? mbedtls_pk_check_pair(&certificate.pk, &private_key, mbedtls_ctr_drbg_random, random)
            : -1;
#else
    const auto key_result = mbedtls_pk_parse_key(&private_key,
        reinterpret_cast<const unsigned char*>(candidate.private_key_pem.data()),
        candidate.private_key_size + 1U, nullptr, 0U);
    const auto pair_result = cert_result == 0 && key_result == 0
                                 ? mbedtls_pk_check_pair(&certificate.pk, &private_key)
                                 : -1;
#endif
    mbedtls_pk_free(&private_key);
    mbedtls_x509_crt_free(&certificate);
    return cert_result == 0 && key_result == 0 && pair_result == 0;
}

/** @brief Loads and verifies the atomic persistent identity record. */
IdentityReadResult load_identity(platform::WebTlsIdentity& destination) {
    filesystem::FileReader reader{};
    const auto open = filesystem::open_reader(kIdentityPath, reader);
    if (open != filesystem::ReadResult::succeeded)
        return open == filesystem::ReadResult::not_found ? IdentityReadResult::not_found
                                                         : IdentityReadResult::io_error;

    std::array<std::uint8_t, kHeaderSize> header{};
    auto result = read_exact(reader, header.data(), header.size());
    if (result == IdentityReadResult::succeeded &&
        (std::memcmp(header.data(), "NMTH", 4U) != 0 || header[4] != kIdentityVersion ||
            header[5] != 0U))
        result = IdentityReadResult::corrupt;
    const auto cert_size =
        result == IdentityReadResult::succeeded ? read_u16(header.data() + 6U) : 0U;
    const auto key_size =
        result == IdentityReadResult::succeeded ? read_u16(header.data() + 8U) : 0U;
    if (result == IdentityReadResult::succeeded &&
        (cert_size == 0U || cert_size >= destination.certificate_pem.size() || key_size == 0U ||
            key_size >= destination.private_key_pem.size()))
        result = IdentityReadResult::corrupt;
    if (result == IdentityReadResult::succeeded)
        result = read_exact(reader, destination.certificate_pem.data(), cert_size);
    if (result == IdentityReadResult::succeeded)
        result = read_exact(reader, destination.private_key_pem.data(), key_size);
    std::array<std::uint8_t, kChecksumSize> checksum{};
    if (result == IdentityReadResult::succeeded)
        result = read_exact(reader, checksum.data(), checksum.size());
    std::uint8_t trailing = 0U;
    std::size_t trailing_size = 0U;
    if (result == IdentityReadResult::succeeded) {
        result = filesystem::read(reader, &trailing, sizeof(trailing), trailing_size) ==
                         filesystem::ReadResult::succeeded
                     ? (trailing_size == 0U ? IdentityReadResult::succeeded
                                            : IdentityReadResult::corrupt)
                     : IdentityReadResult::io_error;
    }
    const bool closed = filesystem::close(reader);
    if (!closed)
        return IdentityReadResult::io_error;
    if (result != IdentityReadResult::succeeded)
        return result;

    destination.certificate_size = cert_size;
    destination.private_key_size = key_size;
    auto crc = support::crc32_update(support::kCrc32InitialState, header.data(), header.size());
    crc = support::crc32_update(crc, destination.certificate_pem.data(), cert_size);
    crc = support::crc32_update(crc, destination.private_key_pem.data(), key_size);
    if (support::crc32_finalize(crc) != read_u32(checksum.data()))
        return IdentityReadResult::corrupt;
    return IdentityReadResult::succeeded;
}

/** @brief Writes the identity into a staged record and atomically renames it. */
bool persist_identity(const platform::WebTlsIdentity& source) {
    if (filesystem::ensure_directory(kDirectory) != filesystem::MutationResult::succeeded)
        return false;
    std::array<std::uint8_t, kHeaderSize> header{
        'N', 'M', 'T', 'H', kIdentityVersion, 0U, 0U, 0U, 0U, 0U};
    write_u16(header.data() + 6U, static_cast<std::uint16_t>(source.certificate_size));
    write_u16(header.data() + 8U, static_cast<std::uint16_t>(source.private_key_size));
    auto crc = support::crc32_update(support::kCrc32InitialState, header.data(), header.size());
    crc = support::crc32_update(crc, source.certificate_pem.data(), source.certificate_size);
    crc = support::crc32_update(crc, source.private_key_pem.data(), source.private_key_size);
    std::array<std::uint8_t, kChecksumSize> checksum{};
    write_u32(checksum.data(), support::crc32_finalize(crc));

    filesystem::FileWriter writer{};
    if (filesystem::open_writer(kTemporaryPath, writer) != filesystem::MutationResult::succeeded)
        return false;
    bool written = filesystem::write(writer, header.data(), header.size()) ==
                       filesystem::MutationResult::succeeded &&
                   filesystem::write(writer, source.certificate_pem.data(),
                       source.certificate_size) == filesystem::MutationResult::succeeded &&
                   filesystem::write(writer, source.private_key_pem.data(),
                       source.private_key_size) == filesystem::MutationResult::succeeded &&
                   filesystem::write(writer, checksum.data(), checksum.size()) ==
                       filesystem::MutationResult::succeeded;
    written = filesystem::close(writer) && written;
    return written && filesystem::rename(kTemporaryPath, kIdentityPath) ==
                          filesystem::MutationResult::succeeded;
}

/** @brief Sets a certificate serial using the API available in this SDK. */
bool set_certificate_serial(
    mbedtls_x509write_cert* certificate, unsigned char* serial, std::size_t serial_size);

/** @brief Creates a random P-256 key and its self-signed HTTPS server certificate. */
bool generate_identity(platform::WebTlsIdentity& destination) {
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_ctr_drbg_context random;
    mbedtls_ctr_drbg_init(&random);
    static constexpr unsigned char personalization[] = "Newmota HTTPS device identity v1";
    bool success = mbedtls_ctr_drbg_seed(&random, entropy, nullptr, personalization,
                       sizeof(personalization) - 1U) == 0;
#else
    bool success = initialize_psa();
#endif
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
#if MBEDTLS_VERSION_MAJOR >= 4
    psa_key_id_t key_id = PSA_KEY_ID_NULL;
    if (success) {
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
        psa_set_key_bits(&attributes, 256U);
        psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
        psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
        success = initialize_psa() && psa_generate_key(&attributes, &key_id) == PSA_SUCCESS &&
                  mbedtls_pk_wrap_psa(&key, key_id) == 0;
        psa_reset_key_attributes(&attributes);
    }
#else
    if (success) {
        success = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0 &&
                  mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
                      mbedtls_ctr_drbg_random, &random) == 0;
    }
#endif
    if (success) {
        success = mbedtls_pk_write_key_pem(&key,
                      reinterpret_cast<unsigned char*>(destination.private_key_pem.data()),
                      destination.private_key_pem.size()) == 0;
        if (success)
            destination.private_key_size = std::strlen(destination.private_key_pem.data());
    }

    mbedtls_x509write_cert certificate;
    mbedtls_x509write_crt_init(&certificate);
    mbedtls_x509write_crt_set_version(&certificate, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&certificate, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&certificate, &key);
    mbedtls_x509write_crt_set_issuer_key(&certificate, &key);
    unsigned char serial[] = {0x01U};
    std::array<unsigned char, 160> san{};
    std::size_t san_size = 0U;
    if (success) {
        success =
            mbedtls_x509write_crt_set_subject_name(&certificate, "CN=Newmota device,O=Newmota") ==
                0 &&
            mbedtls_x509write_crt_set_issuer_name(&certificate, "CN=Newmota device,O=Newmota") ==
                0 &&
            set_certificate_serial(&certificate, serial, sizeof(serial)) &&
            mbedtls_x509write_crt_set_validity(&certificate, "20200101000000", "20491231235959") ==
                0 &&
            mbedtls_x509write_crt_set_basic_constraints(&certificate, 0, -1) == 0 &&
            mbedtls_x509write_crt_set_key_usage(&certificate, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) ==
                0 &&
            subject_alt_name(san, san_size) &&
            mbedtls_x509write_crt_set_extension(
                &certificate, "\x55\x1d\x11", 3U, 0, san.data(), san_size) == 0;
    }
#if MBEDTLS_VERSION_MAJOR >= 4
    if (success) {
        success = mbedtls_x509write_crt_pem(&certificate,
                      reinterpret_cast<unsigned char*>(destination.certificate_pem.data()),
                      destination.certificate_pem.size()) == 0;
    }
#else
    if (success) {
        success = mbedtls_x509write_crt_pem(&certificate,
                      reinterpret_cast<unsigned char*>(destination.certificate_pem.data()),
                      destination.certificate_pem.size(), mbedtls_ctr_drbg_random, &random) == 0;
    }
#endif
    if (success)
        destination.certificate_size = std::strlen(destination.certificate_pem.data());
    mbedtls_x509write_crt_free(&certificate);
    mbedtls_pk_free(&key);
#if MBEDTLS_VERSION_MAJOR >= 4
    if (key_id != PSA_KEY_ID_NULL)
        (void)psa_destroy_key(key_id);
#else
    mbedtls_ctr_drbg_free(&random);
#endif
    return success;
}

/** @brief Sets the small positive certificate serial across supported Mbed TLS versions. */
bool set_certificate_serial(
    mbedtls_x509write_cert* certificate, unsigned char* serial, const std::size_t serial_size) {
#if MBEDTLS_VERSION_NUMBER >= 0x03060000
    return mbedtls_x509write_crt_set_serial_raw(certificate, serial, serial_size) == 0;
#else
    mbedtls_mpi value;
    mbedtls_mpi_init(&value);
    const auto result = mbedtls_mpi_read_binary(&value, serial, serial_size) == 0 &&
                        mbedtls_x509write_crt_set_serial(certificate, &value) == 0;
    mbedtls_mpi_free(&value);
    return result;
#endif
}

/** @brief Clears candidate credentials after a failed load or generation attempt. */
void clear_identity(platform::WebTlsIdentity& candidate) {
    auto* bytes = reinterpret_cast<volatile unsigned char*>(&candidate);
    for (std::size_t index = 0U; index < sizeof(candidate); ++index)
        bytes[index] = 0U;
}

} // namespace

/** @brief Loads or atomically creates the persistent HTTPS identity. */
bool prepare_web_tls_identity() {
    if (identity_ready)
        return true;
    clear_identity(identity);
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_ctr_drbg_context random;
    mbedtls_ctr_drbg_init(&random);
    static constexpr unsigned char personalization[] = "Newmota HTTPS identity validation";
    const bool random_ready = mbedtls_ctr_drbg_seed(&random, entropy, nullptr, personalization,
                                  sizeof(personalization) - 1U) == 0;
#else
    std::array<unsigned char, 32> entropy_probe{};
    const bool random_ready =
        platform::web_random(entropy_probe.data(), entropy_probe.size()) && initialize_psa();
    volatile unsigned char* probe = entropy_probe.data();
    for (std::size_t index = 0U; index < entropy_probe.size(); ++index)
        probe[index] = 0U;
#endif
    const auto loaded = random_ready ? load_identity(identity) : IdentityReadResult::io_error;
    bool valid = random_ready && loaded == IdentityReadResult::succeeded &&
#if MBEDTLS_VERSION_MAJOR < 4
                 validate_identity(identity, &random);
#else
                 validate_identity(identity);
#endif
    if (random_ready &&
        (loaded == IdentityReadResult::not_found || loaded == IdentityReadResult::corrupt ||
            (loaded == IdentityReadResult::succeeded && !valid))) {
        clear_identity(identity);
#if MBEDTLS_VERSION_MAJOR < 4
        valid = generate_identity(identity) && validate_identity(identity, &random) &&
                persist_identity(identity);
#else
        valid = generate_identity(identity) && validate_identity(identity) &&
                persist_identity(identity);
#endif
    }
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_ctr_drbg_free(&random);
#endif
    if (!valid) {
        clear_identity(identity);
        return false;
    }
    identity_ready = true;
    return true;
}

/** @brief Returns the device identity after successful preparation. */
const platform::WebTlsIdentity& web_tls_identity() {
    return identity;
}

} // namespace newmota
