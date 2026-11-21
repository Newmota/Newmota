/*
  update.h - platform-owned native firmware update writer contract

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace newmota::platform {

enum class UpdateWriterResult : std::uint8_t {
    success,
    unavailable,
    busy,
    rejected,
    failed,
};

/**
 * @brief Native image writer owned by the selected platform adapter.
 *
 * begin() atomically reserves one target and records the expected image length.
 * It returns busy without changing an existing lease, and any other failure must
 * leave no lease owned by the caller. It must not erase protected flash or the
 * target before write(0, first_bytes) has checked the native image prefix.
 * finish() verifies the complete native image without selecting it; activate()
 * is the only operation that changes the next boot. abort() releases only the
 * caller's active lease and is safe after failure or more than once.
 */
class UpdateWriter {
  public:
    virtual ~UpdateWriter() = default;
    [[nodiscard]] virtual bool available() const = 0;
    [[nodiscard]] virtual std::uint32_t maximum_image_size() const = 0;
    [[nodiscard]] virtual std::size_t maximum_chunk_size() const = 0;
    /** @brief Minimum prefix bytes required in the first write() call. */
    [[nodiscard]] virtual std::size_t minimum_initial_chunk_size() const = 0;
    [[nodiscard]] virtual UpdateWriterResult begin(std::uint32_t image_size) = 0;
    [[nodiscard]] virtual UpdateWriterResult write(
        std::uint32_t offset, std::string_view bytes) = 0;
    [[nodiscard]] virtual UpdateWriterResult finish() = 0;
    [[nodiscard]] virtual UpdateWriterResult activate() = 0;
    virtual void abort() = 0;
};

/** @brief Returns the writer selected for this image's browser update route. */
[[nodiscard]] UpdateWriter& browser_update_writer();

} // namespace newmota::platform
