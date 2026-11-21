/*
  url_update_limits.h - bounded OTA transport and writer chunk limits

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstddef>

namespace newmota::url_update_detail {

inline constexpr std::size_t kBodyBufferCapacity = 1024U;

/** @brief Caps native writer chunks to the fixed URL transport buffer. */
[[nodiscard]] constexpr std::size_t bounded_writer_chunk_capacity(const std::size_t writer_maximum,
    const std::size_t buffer_capacity = kBodyBufferCapacity) noexcept {
    return writer_maximum < buffer_capacity ? writer_maximum : buffer_capacity;
}

/** @brief Checks that the required first image prefix fits one bounded write. */
[[nodiscard]] constexpr bool writer_chunk_limits_are_usable(const std::size_t writer_maximum,
    const std::size_t initial_prefix,
    const std::size_t buffer_capacity = kBodyBufferCapacity) noexcept {
    const auto capacity = bounded_writer_chunk_capacity(writer_maximum, buffer_capacity);
    return capacity != 0U && initial_prefix != 0U && initial_prefix <= capacity;
}

} // namespace newmota::url_update_detail
