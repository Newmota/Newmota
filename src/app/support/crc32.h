/*
  crc32.h - shared reflected IEEE CRC-32 contract for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace newmota::support {

inline constexpr std::uint32_t kCrc32InitialState = UINT32_MAX;

/**
 * @brief Updates a raw reflected IEEE CRC-32 state.
 * @param state Initial state or the raw result of a preceding update.
 * @param data Bytes to include; may be null only when @p size is zero.
 * @param size Number of bytes to include.
 * @return The raw, unfinalized state for continued incremental use.
 */
[[nodiscard]] std::uint32_t crc32_update(std::uint32_t state, const void* data, std::size_t size);

/**
 * @brief Applies the standard final XOR to a raw CRC-32 state.
 */
[[nodiscard]] constexpr std::uint32_t crc32_finalize(const std::uint32_t state) {
    return state ^ UINT32_MAX;
}

} // namespace newmota::support
