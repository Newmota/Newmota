/*
  tasmota_settings.h - bounded ESP Tasmota settings decoder

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace newmota::migration {

inline constexpr std::size_t kTasmotaSettingsSize = 4096;
inline constexpr std::size_t kTasmotaWifiSlotCount = 2;
inline constexpr std::size_t kTasmotaSsidCapacity = 32;
inline constexpr std::size_t kTasmotaPasswordCapacity = 64;

struct TasmotaCredentials final {
    std::array<std::array<char, kTasmotaSsidCapacity>, kTasmotaWifiSlotCount> ssids{};
    std::array<std::uint8_t, kTasmotaWifiSlotCount> ssid_lengths{};
    std::array<std::array<char, kTasmotaPasswordCapacity>, kTasmotaWifiSlotCount> passwords{};
    std::array<std::uint8_t, kTasmotaWifiSlotCount> password_lengths{};
    std::array<char, kTasmotaPasswordCapacity> web_password{};
    std::uint8_t web_password_length{};
    std::uint8_t preferred_slot{};
};

enum class TasmotaSettingsParseResult : std::uint8_t {
    valid,
    unsupported,
    invalid,
};

/**
 * @brief Decodes credentials shared by eight ESP target layouts in Tasmota 15.5.0.2.
 * @return Whether the bounded record is valid, unsupported, or malformed.
 */
[[nodiscard]] TasmotaSettingsParseResult parse_tasmota_settings(
    const std::uint8_t* bytes, std::size_t size, TasmotaCredentials& settings);

} // namespace newmota::migration
