/*
  update_session.h - bounded firmware update session contract

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "newmota/platform/update.h"

#include <cstdint>
#include <string_view>

namespace newmota {

enum class UpdateSessionState : std::uint8_t {
    idle,
    receiving,
    verified,
    activated,
    failed,
};

enum class UpdateSessionResult : std::uint8_t {
    success,
    unavailable,
    busy,
    invalid_state,
    invalid_size,
    invalid_offset,
    invalid_chunk,
    incomplete,
    image_rejected,
    io_error,
};

/** @brief One allocation-free sequential update lease around a native writer. */
class UpdateSession final {
  public:
    explicit UpdateSession(platform::UpdateWriter& writer) : writer_(writer) {}

    UpdateSession(const UpdateSession&) = delete;
    UpdateSession& operator=(const UpdateSession&) = delete;
    UpdateSession(UpdateSession&&) = delete;
    UpdateSession& operator=(UpdateSession&&) = delete;

    /** @brief Reserves a native target without erasing it. */
    [[nodiscard]] UpdateSessionResult begin(std::uint32_t image_size);
    /** @brief Accepts only the next bounded bytes in the declared image. */
    [[nodiscard]] UpdateSessionResult write(std::uint32_t offset, std::string_view bytes);
    /** @brief Verifies a complete image while leaving it unselected. */
    [[nodiscard]] UpdateSessionResult finish();
    /** @brief Selects a previously verified image for the next boot. */
    [[nodiscard]] UpdateSessionResult activate();
    /** @brief Discards an in-progress or verified native update. */
    [[nodiscard]] UpdateSessionResult abort();

    [[nodiscard]] UpdateSessionState state() const { return state_; }

    [[nodiscard]] std::uint32_t received_bytes() const { return received_bytes_; }

    [[nodiscard]] std::uint32_t expected_bytes() const { return expected_bytes_; }

    [[nodiscard]] bool writer_available() const { return writer_.available(); }

    [[nodiscard]] std::uint32_t maximum_image_size() const { return writer_.maximum_image_size(); }

    [[nodiscard]] std::size_t maximum_chunk_size() const { return writer_.maximum_chunk_size(); }

    [[nodiscard]] std::size_t minimum_initial_chunk_size() const {
        return writer_.minimum_initial_chunk_size();
    }

  private:
    [[nodiscard]] UpdateSessionResult writer_result(platform::UpdateWriterResult result) const;
    [[nodiscard]] UpdateSessionResult fail(UpdateSessionResult result);

    platform::UpdateWriter& writer_;
    UpdateSessionState state_{UpdateSessionState::idle};
    bool owns_writer_lease_{};
    std::uint32_t expected_bytes_{};
    std::uint32_t received_bytes_{};
};

} // namespace newmota
