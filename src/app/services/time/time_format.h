/*
  time_format.h - bounded portable civil-time formats

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file time_format.h
 * @brief Bounded portable civil-time formats.
 */

#pragma once
#include <cstddef>
#include <string_view>

#include "services/time/time_service.h"

namespace newmota {
enum class TimeFormatStatus { ok, invalid_format, mismatch, overflow };
// Fixed English calendar names; epochs are naive civil seconds with no timezone conversion.
// Formats are limited to 128 bytes, parse inputs to 256, and output to 255 plus NUL.
TimeFormatStatus format_time(std::string_view format, std::uint32_t epoch, char* output,
    std::size_t capacity, std::size_t& size);
TimeFormatStatus parse_time(
    std::string_view input, std::string_view format, std::uint32_t& epoch, std::size_t& consumed);
} // namespace newmota
