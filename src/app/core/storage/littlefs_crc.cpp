/*
  littlefs_crc.cpp - Newmota CRC adapter for LittleFS

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "support/crc32.h"

#include "lfs_util.h"

#include <cstddef>
#include <cstdint>

/**
 * @brief Updates LittleFS's raw CRC state through the selected Newmota backend.
 */
extern "C" std::uint32_t lfs_crc(
    const std::uint32_t state, const void* const data, const std::size_t size) {
    return newmota::support::crc32_update(state, data, size);
}
