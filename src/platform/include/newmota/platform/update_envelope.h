/*
  update_envelope.h - unsigned target and layout metadata for OTA payloads

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "support/crc32.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace newmota::platform::update_detail {

inline constexpr std::size_t kEnvelopeSize = 72U;
inline constexpr std::size_t kEnvelopeDigestSize = 16U;
inline constexpr char kEnvelopeMagic[8] = {'N', 'W', 'M', 'O', 'T', 'A', '1', '\0'};
inline constexpr std::uint16_t kEnvelopeSchema = 1U;

#pragma pack(push, 1)

struct UpdateEnvelopeV1 final {
    char magic[8];
    std::uint16_t schema;
    std::uint16_t header_size;
    std::uint32_t payload_size;
    std::uint32_t payload_crc32;
    std::uint32_t chip_id;
    std::uint32_t flash_bytes;
    std::uint8_t layout_digest[kEnvelopeDigestSize];
    std::uint32_t application_offset;
    std::uint32_t application_size;
    std::uint32_t safeboot_offset;
    std::uint32_t safeboot_size;
    std::uint32_t filesystem_offset;
    std::uint32_t filesystem_size;
    std::uint32_t header_crc32;
};

#pragma pack(pop)

static_assert(
    sizeof(UpdateEnvelopeV1) == kEnvelopeSize, "Newmota OTA envelope v1 is exactly 72 bytes");

/** @brief Decodes and checks the fixed envelope header before flash changes. */
inline bool decode_envelope(
    const std::string_view prefix, const std::uint32_t artifact_size, UpdateEnvelopeV1& envelope) {
    if (prefix.size() < sizeof(envelope) || artifact_size < sizeof(envelope)) {
        return false;
    }
    std::memcpy(&envelope, prefix.data(), sizeof(envelope));
    if (std::memcmp(envelope.magic, kEnvelopeMagic, sizeof(kEnvelopeMagic)) != 0 ||
        envelope.schema != kEnvelopeSchema || envelope.header_size != sizeof(envelope) ||
        envelope.payload_size != artifact_size - sizeof(envelope)) {
        return false;
    }
    const auto state = support::crc32_update(
        support::kCrc32InitialState, &envelope, offsetof(UpdateEnvelopeV1, header_crc32));
    return support::crc32_finalize(state) == envelope.header_crc32;
}

/** @brief Binds an envelope to the runtime target's compiled flash geometry. */
inline bool envelope_matches(const UpdateEnvelopeV1& envelope, const std::uint32_t chip_id,
    const std::uint32_t flash_bytes, const std::uint8_t* const layout_digest,
    const std::uint32_t application_offset, const std::uint32_t application_size,
    const std::uint32_t safeboot_offset, const std::uint32_t safeboot_size,
    const std::uint32_t filesystem_offset, const std::uint32_t filesystem_size) {
    return envelope.chip_id == chip_id && envelope.flash_bytes == flash_bytes &&
           layout_digest != nullptr &&
           std::memcmp(envelope.layout_digest, layout_digest, kEnvelopeDigestSize) == 0 &&
           envelope.application_offset == application_offset &&
           envelope.application_size == application_size &&
           envelope.safeboot_offset == safeboot_offset && envelope.safeboot_size == safeboot_size &&
           envelope.filesystem_offset == filesystem_offset &&
           envelope.filesystem_size == filesystem_size;
}

/** @brief Decodes the first sixteen bytes of a 64-character SHA-256 string. */
inline bool decode_layout_digest(const char* const text, std::uint8_t* const digest) {
    if (text == nullptr || digest == nullptr || std::strlen(text) != 64U) {
        return false;
    }
    const auto nibble = [](const char character, std::uint8_t& value) {
        if (character >= '0' && character <= '9') {
            value = static_cast<std::uint8_t>(character - '0');
            return true;
        }
        if (character >= 'a' && character <= 'f') {
            value = static_cast<std::uint8_t>(character - 'a' + 10);
            return true;
        }
        if (character >= 'A' && character <= 'F') {
            value = static_cast<std::uint8_t>(character - 'A' + 10);
            return true;
        }
        return false;
    };
    for (std::size_t index = 0; index < kEnvelopeDigestSize; ++index) {
        std::uint8_t high = 0U;
        std::uint8_t low = 0U;
        if (!nibble(text[index * 2U], high) || !nibble(text[index * 2U + 1U], low)) {
            return false;
        }
        digest[index] = static_cast<std::uint8_t>((high << 4U) | low);
    }
    return true;
}

} // namespace newmota::platform::update_detail
