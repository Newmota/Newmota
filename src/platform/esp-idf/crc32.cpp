/*
  crc32.cpp - ESP-IDF ROM CRC-32 backend for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "support/crc32.h"

#include "esp_rom_crc.h"

#include <cstddef>
#include <cstdint>

namespace newmota::support {

static_assert(
    sizeof(std::size_t) <= sizeof(std::uint32_t), "ESP ROM CRC accepts only 32-bit buffer lengths");

/**
 * @brief Updates a raw reflected IEEE CRC-32 state using the ESP ROM routine.
 *
 * ESP-IDF's API complements its state on entry and exit. The surrounding
 * complements preserve Newmota's raw incremental-state contract.
 */
std::uint32_t crc32_update(
    const std::uint32_t state, const void* const data, const std::size_t size) {
    return ~esp_rom_crc32_le(
        ~state, static_cast<const std::uint8_t*>(data), static_cast<std::uint32_t>(size));
}

} // namespace newmota::support
