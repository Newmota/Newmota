/*
  sensor_settings.h - persistent per-driver probe policy for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <array>
#include <cstdint>

namespace newmota {
/** @brief Persisted presentation choices shared by all compiled sensor drivers. */
struct SensorOutputSettings {
    std::uint8_t temperature_resolution{1};
    std::uint8_t humidity_resolution{1};
    std::uint8_t pressure_resolution{1};
    std::int8_t temperature_offset_tenths{};
    std::int8_t humidity_offset_tenths{};
};

/** @brief Persistent probe policy indexed by stable IDs, independent of compiled drivers. */
struct SensorSettings {
    static constexpr unsigned kMaximumDriverId = 128;
    std::array<std::uint8_t, kMaximumDriverId / 8> disabled{};
    SensorOutputSettings output{};

    [[nodiscard]] bool enabled(std::uint8_t id) const {
        return id != 0 && id <= kMaximumDriverId &&
               (disabled[(id - 1U) / 8U] & (1U << ((id - 1U) % 8U))) == 0;
    }

    void set_enabled(std::uint8_t id, bool value) {
        if (id == 0 || id > kMaximumDriverId)
            return;
        auto& byte = disabled[(id - 1U) / 8U];
        const auto bit = static_cast<std::uint8_t>(1U << ((id - 1U) % 8U));
        byte = static_cast<std::uint8_t>(value ? byte & ~bit : byte | bit);
    }
};
} // namespace newmota
