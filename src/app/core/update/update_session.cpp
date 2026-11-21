/*
  update_session.cpp - bounded firmware update session state machine

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file update_session.cpp
 * @brief Bounded firmware update session state machine.
 */

#include "core/update/update_session.h"

namespace newmota {

UpdateSessionResult UpdateSession::writer_result(const platform::UpdateWriterResult result) const {
    switch (result) {
    case platform::UpdateWriterResult::success:
        return UpdateSessionResult::success;
    case platform::UpdateWriterResult::unavailable:
        return UpdateSessionResult::unavailable;
    case platform::UpdateWriterResult::busy:
        return UpdateSessionResult::busy;
    case platform::UpdateWriterResult::rejected:
        return UpdateSessionResult::image_rejected;
    case platform::UpdateWriterResult::failed:
        return UpdateSessionResult::io_error;
    }
    return UpdateSessionResult::io_error;
}

UpdateSessionResult UpdateSession::fail(const UpdateSessionResult result) {
    if (owns_writer_lease_)
        writer_.abort();
    owns_writer_lease_ = false;
    state_ = UpdateSessionState::failed;
    return result;
}

UpdateSessionResult UpdateSession::begin(const std::uint32_t image_size) {
    if (state_ != UpdateSessionState::idle)
        return UpdateSessionResult::busy;
    if (!writer_.available())
        return UpdateSessionResult::unavailable;
    const auto minimum_initial_chunk = writer_.minimum_initial_chunk_size();
    const auto maximum_chunk = writer_.maximum_chunk_size();
    if (image_size == 0U || image_size > writer_.maximum_image_size() || maximum_chunk == 0U ||
        minimum_initial_chunk == 0U || minimum_initial_chunk > maximum_chunk ||
        image_size < minimum_initial_chunk) {
        return UpdateSessionResult::invalid_size;
    }
    const auto result = writer_result(writer_.begin(image_size));
    if (result == UpdateSessionResult::busy)
        return result;
    if (result != UpdateSessionResult::success) {
        state_ = UpdateSessionState::failed;
        return result;
    }
    owns_writer_lease_ = true;
    expected_bytes_ = image_size;
    received_bytes_ = 0U;
    state_ = UpdateSessionState::receiving;
    return UpdateSessionResult::success;
}

UpdateSessionResult UpdateSession::write(const std::uint32_t offset, const std::string_view bytes) {
    if (state_ != UpdateSessionState::receiving)
        return UpdateSessionResult::invalid_state;
    if (bytes.empty() || bytes.size() > writer_.maximum_chunk_size() ||
        (received_bytes_ == 0U && bytes.size() < writer_.minimum_initial_chunk_size()))
        return UpdateSessionResult::invalid_chunk;
    if (offset != received_bytes_)
        return UpdateSessionResult::invalid_offset;
    if (bytes.size() > expected_bytes_ - received_bytes_)
        return UpdateSessionResult::invalid_size;

    const auto result = writer_result(writer_.write(offset, bytes));
    if (result != UpdateSessionResult::success)
        return fail(result);
    received_bytes_ += static_cast<std::uint32_t>(bytes.size());
    return UpdateSessionResult::success;
}

UpdateSessionResult UpdateSession::finish() {
    if (state_ != UpdateSessionState::receiving)
        return UpdateSessionResult::invalid_state;
    if (received_bytes_ != expected_bytes_)
        return UpdateSessionResult::incomplete;

    const auto result = writer_result(writer_.finish());
    if (result != UpdateSessionResult::success)
        return fail(result);
    state_ = UpdateSessionState::verified;
    return UpdateSessionResult::success;
}

UpdateSessionResult UpdateSession::activate() {
    if (state_ != UpdateSessionState::verified)
        return UpdateSessionResult::invalid_state;
    const auto result = writer_result(writer_.activate());
    if (result != UpdateSessionResult::success)
        return result;
    owns_writer_lease_ = false;
    state_ = UpdateSessionState::activated;
    return UpdateSessionResult::success;
}

UpdateSessionResult UpdateSession::abort() {
    if (state_ == UpdateSessionState::activated)
        return UpdateSessionResult::invalid_state;
    if (state_ == UpdateSessionState::idle)
        return UpdateSessionResult::success;
    if (owns_writer_lease_)
        writer_.abort();
    owns_writer_lease_ = false;
    expected_bytes_ = 0U;
    received_bytes_ = 0U;
    state_ = UpdateSessionState::idle;
    return UpdateSessionResult::success;
}

} // namespace newmota
