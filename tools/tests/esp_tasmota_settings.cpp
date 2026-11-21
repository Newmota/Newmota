/*
  esp_tasmota_settings.cpp - host tests for the bounded Tasmota decoder

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/migration/tasmota_settings.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace {

constexpr std::uint16_t kHolder = 4617;
constexpr std::uint32_t kVersion = 0x0f050002U;
constexpr std::size_t kHolderOffset = 0x000;
constexpr std::size_t kSizeOffset = 0x002;
constexpr std::size_t kSaveCounterOffset = 0x004;
constexpr std::size_t kVersionOffset = 0x008;
constexpr std::size_t kTextPoolOffset = 0x017;
constexpr std::size_t kTextPoolSize = 699;
constexpr std::size_t kSsid1TextIndex = 4;
constexpr std::size_t kWebPasswordTextIndex = 10;
constexpr std::size_t kActiveSlotOffset = 0xec8;
constexpr std::size_t kConfigVersionOffset = 0xf36;
constexpr std::size_t kCrcOffset = newmota::migration::kTasmotaSettingsSize - 4U;
// Indices match Tasmota SettingsTextIndex in the supported source version.
constexpr std::array<std::string_view, 11> kTextPoolFields{"old-update-url", "cmnd", "stat", "tele",
    "tasmota-primary", "tasmota-backup", "primary-password", "backup-password", "%topic%", "",
    "tasmota-web-secret"};

using Fixture = std::array<std::uint8_t, newmota::migration::kTasmotaSettingsSize>;

void write_u16(std::uint8_t* const output, const std::uint16_t value) {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8U);
}

void write_u32(std::uint8_t* const output, const std::uint32_t value) {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8U);
    output[2] = static_cast<std::uint8_t>(value >> 16U);
    output[3] = static_cast<std::uint8_t>(value >> 24U);
}

std::uint32_t fixture_crc(const std::uint8_t* const bytes, const std::size_t size) {
    std::uint32_t crc = 0U;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= bytes[index];
        for (std::uint8_t bit = 0; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - static_cast<std::uint32_t>(crc & 1U)));
        }
    }
    return ~crc;
}

std::size_t text_pool_entry_offset(const std::size_t field_index) {
    std::size_t position = kTextPoolOffset;
    for (std::size_t index = 0; index < field_index; ++index) {
        position += kTextPoolFields[index].size() + 1U;
    }
    return position;
}

Fixture make_fixture(
    const std::array<std::string_view, kTextPoolFields.size()>& fields = kTextPoolFields) {
    Fixture bytes{};
    write_u16(bytes.data() + kHolderOffset, kHolder);
    write_u16(bytes.data() + kSizeOffset, static_cast<std::uint16_t>(bytes.size()));
    write_u32(bytes.data() + kSaveCounterOffset, 42U);
    write_u32(bytes.data() + kVersionOffset, kVersion);
    std::size_t pool_position = kTextPoolOffset;
    for (const auto field : fields) {
        if (pool_position + field.size() + 1U > kTextPoolOffset + kTextPoolSize) {
            return {};
        }
        if (!field.empty()) {
            std::memcpy(bytes.data() + pool_position, field.data(), field.size());
        }
        pool_position += field.size() + 1U;
    }
    bytes[kActiveSlotOffset] = 1U;
    bytes[kConfigVersionOffset] = 2U;
    write_u32(bytes.data() + kCrcOffset, fixture_crc(bytes.data(), kCrcOffset));
    return bytes;
}

void expect(const bool condition, const char* const message, int& failures) {
    if (!condition) {
        std::fprintf(stderr, "esp Tasmota settings: %s\n", message);
        ++failures;
    }
}

} // namespace

int main() {
    using newmota::migration::parse_tasmota_settings;
    using newmota::migration::TasmotaCredentials;
    using newmota::migration::TasmotaSettingsParseResult;
    int failures = 0;

    auto fixture = make_fixture();
    TasmotaCredentials settings{};
    expect(parse_tasmota_settings(fixture.data(), fixture.size(), settings) ==
                   TasmotaSettingsParseResult::valid &&
               settings.ssid_lengths[0] == 15U &&
               std::string_view{settings.ssids[0].data(), settings.ssid_lengths[0]} ==
                   "tasmota-primary" &&
               std::string_view{settings.passwords[1].data(), settings.password_lengths[1]} ==
                   "backup-password" &&
               std::string_view{settings.web_password.data(), settings.web_password_length} ==
                   "tasmota-web-secret" &&
               settings.preferred_slot == 1U,
        "variable text pool decodes Wi-Fi and web credentials", failures);

    auto unsupported = make_fixture();
    write_u32(unsupported.data() + kVersionOffset, kVersion - 1U);
    expect(parse_tasmota_settings(unsupported.data(), unsupported.size(), settings) ==
               TasmotaSettingsParseResult::unsupported,
        "different Tasmota version is rejected", failures);

    for (std::uint8_t target = 1U; target <= 8U; ++target) {
        auto supported = make_fixture();
        supported[kConfigVersionOffset] = target;
        write_u32(supported.data() + kCrcOffset, fixture_crc(supported.data(), kCrcOffset));
        expect(parse_tasmota_settings(supported.data(), supported.size(), settings) ==
                       TasmotaSettingsParseResult::valid &&
                   std::string_view{settings.ssids[0].data(), settings.ssid_lengths[0]} ==
                       "tasmota-primary",
            "supported ESP target decodes shared credentials", failures);
    }
    for (const std::uint8_t target : std::array<std::uint8_t, 4>{0U, 9U, 10U, 255U}) {
        unsupported = make_fixture();
        unsupported[kConfigVersionOffset] = target;
        write_u32(unsupported.data() + kCrcOffset, fixture_crc(unsupported.data(), kCrcOffset));
        expect(parse_tasmota_settings(unsupported.data(), unsupported.size(), settings) ==
                   TasmotaSettingsParseResult::unsupported,
            "unknown ESP target layout is rejected", failures);
    }

    auto invalid = make_fixture();
    invalid[0x0200] ^= 1U;
    expect(parse_tasmota_settings(invalid.data(), invalid.size(), settings) ==
               TasmotaSettingsParseResult::invalid,
        "CRC mismatch is rejected", failures);

    invalid = make_fixture();
    write_u32(invalid.data() + kSaveCounterOffset, 0U);
    write_u32(invalid.data() + kCrcOffset, fixture_crc(invalid.data(), kCrcOffset));
    expect(parse_tasmota_settings(invalid.data(), invalid.size(), settings) ==
               TasmotaSettingsParseResult::invalid,
        "zero save counter is rejected", failures);

    auto long_ssid_fields = kTextPoolFields;
    long_ssid_fields[kSsid1TextIndex] = "123456789012345678901234567890123";
    invalid = make_fixture(long_ssid_fields);
    write_u32(invalid.data() + kCrcOffset, fixture_crc(invalid.data(), kCrcOffset));
    expect(parse_tasmota_settings(invalid.data(), invalid.size(), settings) ==
               TasmotaSettingsParseResult::invalid,
        "SSID exceeding its protocol capacity is rejected", failures);

    auto long_web_password_fields = kTextPoolFields;
    long_web_password_fields[kWebPasswordTextIndex] =
        "12345678901234567890123456789012345678901234567890123456789012345";
    auto long_web_password = make_fixture(long_web_password_fields);
    write_u32(
        long_web_password.data() + kCrcOffset, fixture_crc(long_web_password.data(), kCrcOffset));
    expect(parse_tasmota_settings(long_web_password.data(), long_web_password.size(), settings) ==
                   TasmotaSettingsParseResult::valid &&
               std::string_view{settings.ssids[0].data(), settings.ssid_lengths[0]} ==
                   "tasmota-primary" &&
               std::string_view{settings.passwords[0].data(), settings.password_lengths[0]} ==
                   "primary-password" &&
               settings.web_password_length == 0U,
        "over-capacity web password is omitted while Wi-Fi credentials import", failures);

    auto short_web_password_fields = kTextPoolFields;
    short_web_password_fields[kWebPasswordTextIndex] = "short";
    auto short_web_password = make_fixture(short_web_password_fields);
    write_u32(
        short_web_password.data() + kCrcOffset, fixture_crc(short_web_password.data(), kCrcOffset));
    expect(parse_tasmota_settings(short_web_password.data(), short_web_password.size(), settings) ==
                   TasmotaSettingsParseResult::valid &&
               std::string_view{settings.ssids[0].data(), settings.ssid_lengths[0]} ==
                   "tasmota-primary" &&
               std::string_view{settings.passwords[0].data(), settings.password_lengths[0]} ==
                   "primary-password" &&
               settings.web_password_length == 0U,
        "short web password is omitted while Wi-Fi credentials import", failures);

    auto empty_preferred_fields = kTextPoolFields;
    empty_preferred_fields[kSsid1TextIndex] = {};
    invalid = make_fixture(empty_preferred_fields);
    invalid[kActiveSlotOffset] = 0U;
    write_u32(invalid.data() + kCrcOffset, fixture_crc(invalid.data(), kCrcOffset));
    expect(parse_tasmota_settings(invalid.data(), invalid.size(), settings) ==
                   TasmotaSettingsParseResult::valid &&
               settings.preferred_slot == 1U,
        "empty preferred slot falls back to the other saved network", failures);

    auto unterminated = make_fixture();
    const auto first_ssid_offset = text_pool_entry_offset(kSsid1TextIndex);
    std::memset(unterminated.data() + first_ssid_offset, 'x',
        kTextPoolOffset + kTextPoolSize - first_ssid_offset);
    write_u32(unterminated.data() + kCrcOffset, fixture_crc(unterminated.data(), kCrcOffset));
    expect(parse_tasmota_settings(unterminated.data(), unterminated.size(), settings) ==
               TasmotaSettingsParseResult::invalid,
        "missing NUL in selected pool entry is rejected", failures);

    expect(parse_tasmota_settings(fixture.data(), fixture.size() - 1U, settings) ==
               TasmotaSettingsParseResult::invalid,
        "short records are rejected", failures);

    if (failures == 0)
        std::puts("ESP Tasmota settings tests passed");
    return failures == 0 ? 0 : 1;
}
