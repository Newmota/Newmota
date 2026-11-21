/*
  timer_settings.h - persistent civil timers for Newmota

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

namespace newmota {
/** @brief One persisted Tasmota timer; solar times use its 12-hour sign encoding. */
struct TimerSettings final {
    static constexpr std::uint8_t kCount = 16;
    static constexpr std::size_t kRecordSize = 11 + kCount * 9;

    struct Entry final {
        std::uint16_t time{};
        std::uint8_t window{};
        std::uint8_t days{};
        std::uint8_t output{1};
        std::uint8_t action{};
        std::uint8_t mode{};
        bool enabled{};
        bool repeat{};

        /** @brief Checks the bounded timer wire model. */
        [[nodiscard]] bool valid() const {
            return time < 1440 && window <= 15 && days <= 127 && output >= 1 && output <= 16 &&
                   action <= 3 && mode <= 2;
        }

        bool operator==(const Entry& other) const {
            return time == other.time && window == other.window && days == other.days &&
                   output == other.output && action == other.action && mode == other.mode &&
                   enabled == other.enabled && repeat == other.repeat;
        }
    };

    std::array<Entry, kCount> entries{};
    std::int32_t latitude{48'858'360};
    std::int32_t longitude{2'294'442};
    std::uint8_t sunrise{};
    bool enabled{};

    /** @brief Validates all records and coordinates in millionths of a degree. */
    [[nodiscard]] bool valid() const {
        if (latitude < -90'000'000 || latitude > 90'000'000 || longitude < -180'000'000 ||
            longitude > 180'000'000 || sunrise > 3)
            return false;
        for (const auto& entry : entries)
            if (!entry.valid())
                return false;
        return true;
    }

    bool operator==(const TimerSettings& other) const {
        return entries == other.entries && latitude == other.latitude &&
               longitude == other.longitude && sunrise == other.sunrise && enabled == other.enabled;
    }
};
} // namespace newmota
