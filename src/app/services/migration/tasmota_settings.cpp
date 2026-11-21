/*
  tasmota_settings.cpp - bounded ESP Tasmota settings decoder

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file tasmota_settings.cpp
 * @brief Validates preserved Tasmota records and imports required Wi-Fi plus compatible optional
 * credentials.
 */

#include "services/migration/tasmota_settings.h"

#include <cstddef>
#include <cstdint>

namespace newmota::migration {
namespace {

constexpr std::uint16_t kStockConfigHolder = 4617;
constexpr std::uint32_t kTasmotaVersion = 0x0f050002U;

// Tasmota 15.5.0.2 (37fed605bade755234b04e1076afbc31a7699eef) ESP
// targets share the imported fields at these offsets.
// Target-specific GPIO and template fields are intentionally not decoded.
[[nodiscard]] constexpr bool supported_esp_config_version(const std::uint8_t version) {
    switch (version) {
    case 1U: // ESP32
    case 2U: // ESP32-S3
    case 3U: // ESP32-S2
    case 4U: // ESP32-C3
    case 5U: // ESP32-C2
    case 6U: // ESP32-C6
    case 7U: // ESP32-P4
    case 8U: // ESP32-C5
        return true;
    default:
        return false;
    }
}

constexpr std::size_t kHolderOffset = 0x000;
constexpr std::size_t kSizeOffset = 0x002;
constexpr std::size_t kSaveCounterOffset = 0x004;
constexpr std::size_t kVersionOffset = 0x008;
constexpr std::size_t kTextPoolOffset = 0x017;
constexpr std::size_t kTextPoolSize = 699;
constexpr std::size_t kTextPoolEnd = kTextPoolOffset + kTextPoolSize;
// Tasmota SettingsTextIndex: OTA URL, three MQTT prefixes, two SSIDs,
// two passwords, hostname, syslog host, and web password.
constexpr std::size_t kFirstSsidIndex = 4;
constexpr std::size_t kFirstPostWifiIndex = 8;
constexpr std::size_t kWebPasswordIndex = 10;
constexpr std::size_t kActiveSlotOffset = 0xec8;
constexpr std::size_t kEspConfigVersionOffset = 0xf36;
constexpr std::size_t kCrcOffset = kTasmotaSettingsSize - sizeof(std::uint32_t);

static_assert(kTextPoolEnd == 0x2d2);
static_assert(kTextPoolEnd <= kCrcOffset);

[[nodiscard]] std::uint16_t read_u16(const std::uint8_t* const source) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(source[0]) | (static_cast<std::uint16_t>(source[1]) << 8U));
}

[[nodiscard]] std::uint32_t read_u32(const std::uint8_t* const source) {
    return static_cast<std::uint32_t>(source[0]) | (static_cast<std::uint32_t>(source[1]) << 8U) |
           (static_cast<std::uint32_t>(source[2]) << 16U) |
           (static_cast<std::uint32_t>(source[3]) << 24U);
}

/** @brief Matches Tasmota's reflected CRC-32 parameters for TSettings. */
[[nodiscard]] std::uint32_t tasmota_crc32(const std::uint8_t* const bytes, const std::size_t size) {
    std::uint32_t crc = 0U;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= bytes[index];
        for (std::uint8_t bit = 0; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - static_cast<std::uint32_t>(crc & 1U)));
        }
    }
    return ~crc;
}

[[nodiscard]] bool skip_text_pool_entry(const std::uint8_t* const bytes, std::size_t& position) {
    while (position < kTextPoolEnd && bytes[position] != 0U) {
        ++position;
    }
    if (position == kTextPoolEnd) {
        return false;
    }
    ++position;
    return true;
}

template <std::size_t OutputSize>
[[nodiscard]] bool read_text_pool_entry(const std::uint8_t* const bytes, std::size_t& position,
    const std::size_t maximum_size, std::array<char, OutputSize>& output,
    std::uint8_t& output_size) {
    const auto start = position;
    while (position < kTextPoolEnd && bytes[position] != 0U) {
        const auto character = bytes[position];
        if (character == '\r' || character == '\n') {
            return false;
        }
        ++position;
    }
    if (position == kTextPoolEnd) {
        return false;
    }
    const auto length = position - start;
    ++position;
    if (length > maximum_size || length > OutputSize) {
        return false;
    }

    output.fill('\0');
    for (std::size_t index = 0; index < length; ++index) {
        output[index] = static_cast<char>(bytes[start + index]);
    }
    output_size = static_cast<std::uint8_t>(length);
    return true;
}

} // namespace

TasmotaSettingsParseResult parse_tasmota_settings(
    const std::uint8_t* const bytes, const std::size_t size, TasmotaCredentials& settings) {
    settings = {};
    if (bytes == nullptr || size != kTasmotaSettingsSize) {
        return TasmotaSettingsParseResult::invalid;
    }
    if (read_u16(bytes + kHolderOffset) != kStockConfigHolder ||
        read_u16(bytes + kSizeOffset) != kTasmotaSettingsSize ||
        read_u32(bytes + kVersionOffset) != kTasmotaVersion ||
        !supported_esp_config_version(bytes[kEspConfigVersionOffset])) {
        return TasmotaSettingsParseResult::unsupported;
    }

    const auto save_counter = read_u32(bytes + kSaveCounterOffset);
    if (save_counter == 0U || read_u32(bytes + kCrcOffset) != tasmota_crc32(bytes, kCrcOffset) ||
        bytes[kActiveSlotOffset] >= kTasmotaWifiSlotCount) {
        return TasmotaSettingsParseResult::invalid;
    }

    auto text_position = kTextPoolOffset;
    for (std::size_t index = 0; index < kFirstSsidIndex; ++index) {
        if (!skip_text_pool_entry(bytes, text_position)) {
            return TasmotaSettingsParseResult::invalid;
        }
    }
    if (!read_text_pool_entry(bytes, text_position, kTasmotaSsidCapacity, settings.ssids[0],
            settings.ssid_lengths[0]) ||
        !read_text_pool_entry(bytes, text_position, kTasmotaSsidCapacity, settings.ssids[1],
            settings.ssid_lengths[1]) ||
        !read_text_pool_entry(bytes, text_position, kTasmotaPasswordCapacity, settings.passwords[0],
            settings.password_lengths[0]) ||
        !read_text_pool_entry(bytes, text_position, kTasmotaPasswordCapacity, settings.passwords[1],
            settings.password_lengths[1])) {
        settings = {};
        return TasmotaSettingsParseResult::invalid;
    }
    for (std::size_t index = kFirstPostWifiIndex; index < kWebPasswordIndex; ++index) {
        if (!skip_text_pool_entry(bytes, text_position)) {
            settings = {};
            return TasmotaSettingsParseResult::invalid;
        }
    }
    /** @brief Validate optional password structure without letting incompatible policy block Wi-Fi.
     * Unterminated/newline-bearing text remains malformed; a well-formed password outside
     * Newmota's 8..64-byte policy is omitted rather than rejecting required credentials.
     */
    const auto web_start = text_position;
    if (!skip_text_pool_entry(bytes, text_position)) {
        settings = {};
        return TasmotaSettingsParseResult::invalid;
    }
    const auto web_length = text_position - web_start - 1U;
    for (std::size_t index = 0; index < web_length; ++index) {
        if (bytes[web_start + index] == '\r' || bytes[web_start + index] == '\n') {
            settings = {};
            return TasmotaSettingsParseResult::invalid;
        }
    }
    if (web_length >= 8U && web_length <= settings.web_password.size()) {
        for (std::size_t index = 0; index < web_length; ++index)
            settings.web_password[index] = static_cast<char>(bytes[web_start + index]);
        settings.web_password_length = static_cast<std::uint8_t>(web_length);
    }

    for (std::size_t slot = 0; slot < kTasmotaWifiSlotCount; ++slot) {
        if (settings.ssid_lengths[slot] == 0U) {
            settings.passwords[slot].fill('\0');
            settings.password_lengths[slot] = 0U;
        }
    }

    settings.preferred_slot = bytes[kActiveSlotOffset];
    const auto preferred = settings.preferred_slot;
    const auto alternate = static_cast<std::uint8_t>(preferred == 0U ? 1U : 0U);
    if (settings.ssid_lengths[preferred] == 0U && settings.ssid_lengths[alternate] != 0U) {
        settings.preferred_slot = alternate;
    }
    return TasmotaSettingsParseResult::valid;
}

} // namespace newmota::migration
