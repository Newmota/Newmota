/*
  utc_calendar.h - Pure Gregorian UTC conversion

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file utc_calendar.h
 * @brief Checked Gregorian conversion shared by time intake protocols.
 */
#pragma once
#include <cstdint>

namespace newmota::utc_calendar {
/** @brief Shared earliest trusted Unix timestamp for automatic and manual intake. */
inline constexpr std::uint32_t kFirstValidTime = 1451602800U;

/** @brief Converts valid civil UTC to uint32 Unix seconds, rejecting overflow and leap seconds. */
inline bool epoch(unsigned year, unsigned month, unsigned day, unsigned hour, unsigned minute,
    unsigned second, std::uint32_t& output) {
    const auto leap = [](unsigned y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); };
    constexpr unsigned days[]{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 1970 || year > 2106 || month < 1 || month > 12 || day < 1 ||
        day > days[month - 1] + (month == 2 && leap(year)) || hour > 23 || minute > 59 ||
        second > 59)
        return false;
    std::uint64_t count{};
    for (unsigned y = 1970; y < year; ++y)
        count += leap(y) ? 366U : 365U;
    for (unsigned m = 1; m < month; ++m)
        count += days[m - 1] + (m == 2 && leap(year));
    const auto value = (count + day - 1U) * 86400U + hour * 3600U + minute * 60U + second;
    if (value > UINT32_MAX)
        return false;
    output = static_cast<std::uint32_t>(value);
    return true;
}
} // namespace newmota::utc_calendar
