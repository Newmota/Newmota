/*
  flash_access.h - checked Pico physical flash operations

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace newmota::platform::pico_detail {

/**
 * @brief Resolves the addressable flash size once and bounds all later accesses by it.
 *
 * RP2040 accepts a JEDEC-reported size above the catalog size only when no smaller power of two
 * mirrors offset 0. RP2350 keeps the catalog size, which its partition table describes.
 * @return Bytes from offset 0 that flash operations may address.
 */
[[nodiscard]] std::uint32_t flash_capacity();

/** @brief Reads a bounded physical flash range by flash-relative offset. */
[[nodiscard]] bool flash_read(std::uint32_t offset, void* buffer, std::size_t size);

/** @brief Erases complete sectors from physical flash. */
[[nodiscard]] bool flash_erase(std::uint32_t offset, std::size_t size);

/** @brief Programs complete flash pages at a physical offset. */
[[nodiscard]] bool flash_program(std::uint32_t offset, const void* data, std::size_t size);

} // namespace newmota::platform::pico_detail
