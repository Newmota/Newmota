/*
  text.h - allocation-free ASCII text helpers for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <string_view>

namespace newmota::support {

/**
 * @brief Tests whether a character is ASCII whitespace.
 */
[[nodiscard]] constexpr bool ascii_is_space(const char character) {
    return character == ' ' || character == '\t' || character == '\r' || character == '\n' ||
           character == '\f' || character == '\v';
}

/**
 * @brief Converts one ASCII letter to lowercase without locale state.
 */
[[nodiscard]] constexpr char ascii_lower(const char character) {
    return character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A'))
                                                : character;
}

/**
 * @brief Compares two ASCII strings without case sensitivity.
 * @return true when both strings contain the same ASCII characters.
 */
[[nodiscard]] bool ascii_iequals(std::string_view left, std::string_view right);

/**
 * @brief Removes leading and trailing ASCII whitespace from a view.
 */
[[nodiscard]] std::string_view trim_ascii(std::string_view text);

} // namespace newmota::support
