/*
  number.cpp - bounded number parsing helpers for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "support/number.h"

#include <limits>

namespace newmota::support {

/**
 * @brief Parses a non-empty unsigned decimal value within an inclusive bound.
 */
bool parse_unsigned_decimal(
    const std::string_view text, const std::uint32_t maximum, std::uint32_t& value) {
    if (text.empty()) {
        return false;
    }

    std::uint32_t parsed = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint32_t>(character - '0');
        if (parsed > maximum / 10U || (parsed == maximum / 10U && digit > maximum % 10U)) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    value = parsed;
    return true;
}

/**
 * @brief Parses a signed decimal value within inclusive bounds.
 */
bool parse_signed_decimal(std::string_view text, const std::int32_t minimum,
    const std::int32_t maximum, std::int32_t& value) {
    if (text.empty() || minimum > maximum) {
        return false;
    }

    bool negative = false;
    if (text.front() == '+' || text.front() == '-') {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return false;
    }

    const auto negative_limit =
        minimum == std::numeric_limits<std::int32_t>::min()
            ? static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) + 1U
        : minimum < 0 ? static_cast<std::uint32_t>(-minimum)
                      : 0U;
    const auto positive_limit = maximum > 0 ? static_cast<std::uint32_t>(maximum) : 0U;
    std::uint32_t magnitude = 0;
    if (!parse_unsigned_decimal(text, negative ? negative_limit : positive_limit, magnitude)) {
        return false;
    }

    if (negative) {
        value = magnitude == negative_limit && minimum == std::numeric_limits<std::int32_t>::min()
                    ? minimum
                    : -static_cast<std::int32_t>(magnitude);
    } else {
        value = static_cast<std::int32_t>(magnitude);
    }
    if (value < minimum || value > maximum) {
        return false;
    }
    return true;
}

/**
 * @brief Parses an unsigned decimal fraction into tenths, dropping further digits.
 */
bool parse_unsigned_tenths(const std::string_view text, std::uint32_t& value) {
    constexpr auto kMaximum = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t parsed = 0;
    bool any_digit = false;
    bool in_fraction = false;
    bool tenth_seen = false;
    for (const char character : text) {
        if (character == '.' && !in_fraction) {
            in_fraction = true;
            continue;
        }
        if (character < '0' || character > '9') {
            return false;
        }
        any_digit = true;
        if (in_fraction && tenth_seen) {
            continue;
        }
        tenth_seen = in_fraction;
        const auto digit = static_cast<std::uint32_t>(character - '0');
        parsed = parsed > (kMaximum - digit) / 10U ? kMaximum : parsed * 10U + digit;
    }
    if (!any_digit) {
        return false;
    }
    if (!tenth_seen) {
        parsed = parsed > kMaximum / 10U ? kMaximum : parsed * 10U;
    }
    value = parsed;
    return true;
}

} // namespace newmota::support
