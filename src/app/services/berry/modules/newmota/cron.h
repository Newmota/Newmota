/*
  cron.h - bounded portable civil-time cron patterns

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file cron.h
 * @brief Bounded portable civil-time cron patterns.
 */

#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace newmota {
/** @brief Cron field masks; only seconds and minutes need more than 32 bits. */
struct CronPattern {
    std::array<std::uint64_t, 2> seconds_minutes{};
    std::array<std::uint32_t, 4> hours_days_months_weekdays{};
};

/** @brief Parses six cron fields and updates the pattern only on success.
 * @param expression Six-field cron expression.
 * @param pattern Receives the parsed field masks on success.
 * @return True when all fields are valid.
 */
bool parse_cron(std::string_view expression, CronPattern& pattern);
/** @brief Finds the next matching epoch strictly after the supplied value.
 * @param pattern Parsed cron field masks.
 * @param epoch Starting epoch in seconds.
 * @param next Receives the next occurrence, including UINT32_MAX when valid.
 * @return True when a representable occurrence exists.
 */
bool next_cron_after(const CronPattern& pattern, std::uint32_t epoch, std::uint32_t& next);
} // namespace newmota
