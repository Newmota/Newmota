/*
  number.h - bounded number parsing helpers for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <charconv>
#include <cstddef>
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

/** @brief NUL-terminated text of a 64-bit integer, with its length. */
struct IntegerText {
    char text[21];         ///< Up to 20 characters ("-9223372036854775808", UINT64_MAX) and a NUL.
    std::size_t size = 0U; ///< Characters before the NUL.
};

namespace detail {
template <typename Integer> [[nodiscard]] IntegerText decimal_text(Integer value) {
    IntegerText out{};
    out.size = static_cast<std::size_t>(
        std::to_chars(out.text, out.text + sizeof(out.text) - 1, value).ptr - out.text);
    out.text[out.size] = '\0';
    return out;
}
} // namespace detail

/**
 * @brief Formats a 64-bit integer in decimal without printf's `%ll` conversions, which
 * newlib-nano (the Beken C library) does not implement: it prints "lu" instead. Pass `.text`
 * to a `%s` or write `.text` and `.size` directly.
 */
[[nodiscard]] inline IntegerText decimal(std::uint64_t value) {
    return detail::decimal_text(value);
}

/** @copydoc decimal(std::uint64_t) */
[[nodiscard]] inline IntegerText decimal(std::int64_t value) {
    return detail::decimal_text(value);
}

/**
 * @brief Formats an unsigned 64-bit integer in base 10 or 16 (upper case), padded with leading
 * zeros to at least @p width characters (at most 20), as `%0*llu` and `%0*llX` would.
 */
[[nodiscard]] inline IntegerText digits(std::uint64_t value, int base, unsigned width) {
    char raw[20];
    const auto length =
        static_cast<std::size_t>(std::to_chars(raw, raw + sizeof(raw), value, base).ptr - raw);
    const std::size_t total = width > length ? (width < 20U ? width : 20U) : length;
    IntegerText out{};
    std::size_t index = 0;
    for (; index < total - length; ++index)
        out.text[index] = '0';
    for (std::size_t source = 0; source < length; ++source, ++index) {
        const char c = raw[source];
        out.text[index] = c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c;
    }
    out.text[index] = '\0';
    out.size = index;
    return out;
}

} // namespace newmota::support
