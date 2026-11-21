/*
  number.h - bounded number parsing helpers for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstdint>
#include <string_view>

namespace newmota::support {

/**
 * @brief Parses a non-empty unsigned decimal value within an inclusive bound.
 * @param text ASCII decimal digits without a sign or surrounding whitespace.
 * @param maximum Largest accepted value.
 * @param value Receives the parsed value only on success.
 * @return true when the complete input is valid and in range.
 */
[[nodiscard]] bool parse_unsigned_decimal(
    std::string_view text, std::uint32_t maximum, std::uint32_t& value);

/**
 * @brief Parses a signed decimal value within inclusive bounds.
 * @param text ASCII decimal digits with an optional leading sign.
 * @param minimum Smallest accepted value.
 * @param maximum Largest accepted value.
 * @param value Receives the parsed value only on success.
 * @return true when the complete input is valid and in range.
 */
[[nodiscard]] bool parse_signed_decimal(
    std::string_view text, std::int32_t minimum, std::int32_t maximum, std::int32_t& value);

/**
 * @brief Parses an unsigned decimal fraction into tenths, dropping further digits.
 * @param text ASCII digits with at most one '.', and at least one digit.
 * @param value Receives the tenths on success; a value too large for it saturates.
 * @return true when the complete input is valid.
 */
[[nodiscard]] bool parse_unsigned_tenths(std::string_view text, std::uint32_t& value);

} // namespace newmota::support
