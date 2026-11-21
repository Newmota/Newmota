/*
  text.cpp - allocation-free ASCII text helpers for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "support/text.h"

namespace newmota::support {

/**
 * @brief Compares two ASCII strings without case sensitivity.
 */
bool ascii_iequals(const std::string_view left, const std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Removes leading and trailing ASCII whitespace from a view.
 */
std::string_view trim_ascii(std::string_view text) {
    while (!text.empty() && ascii_is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && ascii_is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace newmota::support
