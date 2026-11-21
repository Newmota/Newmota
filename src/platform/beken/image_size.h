/*
  image_size.h - bounded Beken logical linker image measurement

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file image_size.h
 * @brief Bounded Beken logical linker image measurement.
 */

#pragma once

#include <cstdint>

namespace newmota::platform::beken {

[[nodiscard]] inline bool image_size_from_linker_bounds(const std::uintptr_t image_start,
    const std::uintptr_t data_load_start, const std::uintptr_t data_start,
    const std::uintptr_t data_end, const std::uint32_t capacity, std::uint32_t& image_size) {
    if (data_load_start < image_start || data_end < data_start) {
        return false;
    }
    const auto flash_span = data_load_start - image_start;
    const auto data_span = data_end - data_start;
    if (flash_span > capacity || data_span > capacity - flash_span ||
        flash_span + data_span == 0U) {
        return false;
    }
    image_size = static_cast<std::uint32_t>(flash_span + data_span);
    return true;
}

} // namespace newmota::platform::beken
