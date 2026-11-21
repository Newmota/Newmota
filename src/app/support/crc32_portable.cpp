/*
  crc32_portable.cpp - portable reflected IEEE CRC-32 for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "support/crc32.h"

namespace newmota::support {

/**
 * @brief Updates a raw reflected IEEE CRC-32 state in portable C++.
 */
std::uint32_t crc32_update(std::uint32_t state, const void* const data, const std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        state ^= bytes[index];
        for (std::uint8_t bit = 0; bit < 8; ++bit) {
            state = (state >> 1U) ^ (0xedb88320U & (0U - static_cast<std::uint32_t>(state & 1U)));
        }
    }
    return state;
}

} // namespace newmota::support
