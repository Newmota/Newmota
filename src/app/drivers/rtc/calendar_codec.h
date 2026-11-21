/*
  calendar_codec.h - Checked RTC calendar wire conversions

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file calendar_codec.h
 * @brief Pure BCD and weekday conversions shared by the RTC register protocols.
 */
#pragma once
#include "support/utc_calendar.h"
#include <array>

namespace newmota::rtc::calendar {
enum class Weekday : std::uint8_t { one_based, zero_based, one_hot };

/** @brief Seven consecutive registers, with weekday before or after day of month. */
struct Format final {
    bool weekday_first;
    Weekday weekday;
};

inline constexpr Format kDallas{true, Weekday::one_based};
inline constexpr Format kNxp{false, Weekday::zero_based};
inline constexpr Format kEpson{true, Weekday::one_hot};
inline constexpr Format kMicroCrystal{true, Weekday::zero_based};
using Registers = std::array<std::uint8_t, 7>;

/** @brief Rejects reserved bits, invalid decimal nibbles, and out-of-range fields. */
inline bool bcd(std::uint8_t raw, std::uint8_t mask, std::uint8_t minimum, std::uint8_t maximum,
    std::uint8_t& value) {
    if ((raw & ~mask) != 0 || (raw & 15U) > 9 || ((raw >> 4) & 15U) > 9)
        return false;
    value = static_cast<std::uint8_t>((raw >> 4) * 10U + (raw & 15U));
    return value >= minimum && value <= maximum;
}

inline std::uint8_t bcd(std::uint8_t value) {
    return static_cast<std::uint8_t>((value / 10U) * 16U + value % 10U);
}

/** @brief Converts an externally selected hour mode; bit 5 is PM in 12-hour mode. */
inline bool hour(std::uint8_t raw, bool twelve_hour, std::uint8_t& output) {
    if (!twelve_hour)
        return bcd(raw, 0x3f, 0, 23, output);
    if ((raw & 0xc0) != 0 || !bcd(raw & 0x1f, 0x1f, 1, 12, output))
        return false;
    output = static_cast<std::uint8_t>(output % 12U + ((raw & 0x20) ? 12U : 0U));
    return true;
}

/** @brief Converts only 2000-2099; the caller must handle oscillator and century flags. */
inline bool decode(const Registers& raw, Format format, bool twelve_hour, std::uint32_t& epoch) {
    std::uint8_t second{}, minute{}, hours{}, day{}, month{}, year{};
    const auto week_raw = raw[format.weekday_first ? 3 : 4];
    switch (format.weekday) {
    case Weekday::one_based:
        if (week_raw < 1 || week_raw > 7)
            return false;
        break;
    case Weekday::zero_based:
        if (week_raw > 6)
            return false;
        break;
    case Weekday::one_hot:
        if (week_raw == 0 || week_raw > 0x40 || (week_raw & (week_raw - 1U)) != 0)
            return false;
        break;
    }
    if (!bcd(raw[0], 0x7f, 0, 59, second) || !bcd(raw[1], 0x7f, 0, 59, minute) ||
        !hour(raw[2], twelve_hour, hours) ||
        !bcd(raw[format.weekday_first ? 4 : 3], 0x3f, 1, 31, day) ||
        !bcd(raw[5], 0x1f, 1, 12, month) || !bcd(raw[6], 0xff, 0, 99, year))
        return false;
    return utc_calendar::epoch(2000U + year, month, day, hours, minute, second, epoch) &&
           epoch >= utc_calendar::kFirstValidTime;
}

/** @brief Encodes an already validated civil date, preserving an external 12-hour mode. */
inline Registers encode(Format format, bool twelve_hour, std::uint16_t year, std::uint8_t month,
    std::uint8_t day, std::uint8_t hours, std::uint8_t minute, std::uint8_t second,
    std::uint8_t sunday_one) {
    auto weekday = sunday_one;
    if (format.weekday == Weekday::zero_based)
        weekday = static_cast<std::uint8_t>(sunday_one - 1U);
    else if (format.weekday == Weekday::one_hot)
        weekday = static_cast<std::uint8_t>(1U << (sunday_one - 1U));
    const auto encoded_hour =
        twelve_hour ? static_cast<std::uint8_t>(
                          bcd(static_cast<std::uint8_t>(hours % 12U ? hours % 12U : 12U)) |
                          (hours >= 12 ? 0x20 : 0))
                    : bcd(hours);
    return {bcd(second), bcd(minute), encoded_hour, format.weekday_first ? weekday : bcd(day),
        format.weekday_first ? bcd(day) : weekday, bcd(month),
        bcd(static_cast<std::uint8_t>(year - 2000))};
}
} // namespace newmota::rtc::calendar
