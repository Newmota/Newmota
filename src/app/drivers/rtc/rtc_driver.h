/*
  rtc_driver.h - Static RTC chip protocol contract

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file rtc_driver.h
 * @brief Small application-task-only descriptor for independently selected RTC chips.
 */
#pragma once
#include <cstdint>

namespace newmota::rtc {
enum class ReadResult : std::uint8_t { valid, invalid, unavailable, io_error };
enum class WriteResult : std::uint8_t {
    never,
    unchanged,
    success,
    range_error,
    io_error,
    verify_error
};

/** @brief One statically selected chip; invalid calendars still establish presence.
 * Reads return UTC only for a trusted calendar. Writes verify programming before success.
 */
struct Driver final {
    const char* name;
    std::uint8_t address;
    ReadResult (*read)(std::uint8_t bus, std::uint32_t& epoch);
    WriteResult (*write)(std::uint8_t bus, std::uint32_t epoch);
    /** @brief Optional selected-chip setup; never runs on an unclaimed address. */
    bool (*prepare)(std::uint8_t bus){};
};
} // namespace newmota::rtc
