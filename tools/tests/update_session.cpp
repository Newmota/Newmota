/*
  update_session.cpp - host tests for the portable firmware update session

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "core/update/update_session.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace {

void expect(const bool condition, const char* const message, int& failures) {
    if (!condition) {
        std::fprintf(stderr, "update session: %s\n", message);
        ++failures;
    }
}

class FakeWriter final : public newmota::platform::UpdateWriter {
  public:
    bool usable{true};
    bool reject_write{};
    bool reject_begin{};
    bool reject_finish{};
    bool reject_activate{};
    bool began{};
    bool finished{};
    bool activated{};
    bool aborted{};
    bool leased{};
    std::uint32_t image_size{};
    std::size_t minimum_prefix{1U};
    std::array<char, 16> image{};

    [[nodiscard]] bool available() const override { return usable; }

    [[nodiscard]] std::uint32_t maximum_image_size() const override {
        return static_cast<std::uint32_t>(image.size());
    }

    [[nodiscard]] std::size_t maximum_chunk_size() const override { return 4U; }

    [[nodiscard]] std::size_t minimum_initial_chunk_size() const override { return minimum_prefix; }

    [[nodiscard]] newmota::platform::UpdateWriterResult begin(const std::uint32_t size) override {
        if (leased)
            return newmota::platform::UpdateWriterResult::busy;
        if (reject_begin)
            return newmota::platform::UpdateWriterResult::rejected;
        began = true;
        image_size = size;
        leased = true;
        return newmota::platform::UpdateWriterResult::success;
    }

    [[nodiscard]] newmota::platform::UpdateWriterResult write(
        const std::uint32_t offset, const std::string_view bytes) override {
        if (reject_write)
            return newmota::platform::UpdateWriterResult::rejected;
        if (offset + bytes.size() > image.size())
            return newmota::platform::UpdateWriterResult::failed;
        std::memcpy(image.data() + offset, bytes.data(), bytes.size());
        return newmota::platform::UpdateWriterResult::success;
    }

    [[nodiscard]] newmota::platform::UpdateWriterResult finish() override {
        finished = true;
        return reject_finish ? newmota::platform::UpdateWriterResult::rejected
                             : newmota::platform::UpdateWriterResult::success;
    }

    [[nodiscard]] newmota::platform::UpdateWriterResult activate() override {
        if (reject_activate)
            return newmota::platform::UpdateWriterResult::failed;
        activated = true;
        leased = false;
        return newmota::platform::UpdateWriterResult::success;
    }

    void abort() override {
        aborted = true;
        leased = false;
    }
};

static_assert(!std::is_copy_constructible_v<newmota::UpdateSession>);
static_assert(!std::is_move_constructible_v<newmota::UpdateSession>);

} // namespace

int main() {
    using namespace newmota;
    int failures = 0;

    FakeWriter writer;
    UpdateSession session{writer};
    expect(session.begin(10U) == UpdateSessionResult::success && writer.began,
        "one bounded image reserves its writer", failures);
    expect(session.begin(10U) == UpdateSessionResult::busy,
        "a second update cannot take the active lease", failures);
    expect(session.write(1U, "abcd") == UpdateSessionResult::invalid_offset,
        "the first chunk must start at offset zero", failures);
    expect(session.write(0U, "abcde") == UpdateSessionResult::invalid_chunk,
        "a chunk cannot exceed the writer bound", failures);
    expect(session.write(0U, "") == UpdateSessionResult::invalid_chunk, "empty chunks are rejected",
        failures);
    expect(session.write(0U, "abcd") == UpdateSessionResult::success &&
               session.write(4U, "efgh") == UpdateSessionResult::success,
        "sequential bounded chunks are accepted", failures);
    expect(session.finish() == UpdateSessionResult::incomplete, "a short image cannot be verified",
        failures);
    expect(
        session.write(8U, "ij") == UpdateSessionResult::success && session.received_bytes() == 10U,
        "the final chunk is tracked exactly", failures);
    expect(session.finish() == UpdateSessionResult::success &&
               session.state() == UpdateSessionState::verified,
        "a complete image is verified before selection", failures);
    expect(session.activate() == UpdateSessionResult::success && writer.activated &&
               session.state() == UpdateSessionState::activated,
        "activation follows successful verification", failures);
    expect(session.abort() == UpdateSessionResult::invalid_state,
        "an activated image cannot be aborted", failures);

    FakeWriter shared_writer;
    UpdateSession first_owner{shared_writer};
    UpdateSession competing_owner{shared_writer};
    expect(first_owner.begin(4U) == UpdateSessionResult::success &&
               competing_owner.begin(4U) == UpdateSessionResult::busy && !shared_writer.aborted,
        "a competing session cannot abort another owner's lease", failures);
    expect(first_owner.write(0U, "data") == UpdateSessionResult::success &&
               first_owner.finish() == UpdateSessionResult::success &&
               first_owner.activate() == UpdateSessionResult::success,
        "the lease owner remains usable after a competing begin", failures);

    FakeWriter retained_writer;
    retained_writer.reject_begin = true;
    UpdateSession failed_reservation{retained_writer};
    UpdateSession later_owner{retained_writer};
    expect(failed_reservation.begin(4U) == UpdateSessionResult::image_rejected,
        "a rejected reservation acquires no writer lease", failures);
    retained_writer.reject_begin = false;
    expect(later_owner.begin(4U) == UpdateSessionResult::success &&
               failed_reservation.abort() == UpdateSessionResult::success &&
               !retained_writer.aborted &&
               later_owner.write(0U, "data") == UpdateSessionResult::success &&
               later_owner.finish() == UpdateSessionResult::success &&
               later_owner.activate() == UpdateSessionResult::success,
        "a stale failed session cannot abort a later owner's lease", failures);

    FakeWriter prefix_writer;
    prefix_writer.minimum_prefix = 3U;
    UpdateSession prefix_session{prefix_writer};
    expect(prefix_session.begin(2U) == UpdateSessionResult::invalid_size &&
               prefix_session.begin(4U) == UpdateSessionResult::success &&
               prefix_session.write(0U, "ab") == UpdateSessionResult::invalid_chunk &&
               prefix_session.write(0U, "abcd") == UpdateSessionResult::success &&
               prefix_session.finish() == UpdateSessionResult::success &&
               prefix_session.abort() == UpdateSessionResult::success,
        "the first write contains the native image prefix", failures);

    FakeWriter rejecting_writer;
    rejecting_writer.reject_write = true;
    UpdateSession rejected{rejecting_writer};
    expect(rejected.begin(4U) == UpdateSessionResult::success &&
               rejected.write(0U, "bad!") == UpdateSessionResult::image_rejected &&
               rejecting_writer.aborted && rejected.state() == UpdateSessionState::failed,
        "a native image rejection aborts the partial write", failures);
    rejecting_writer.aborted = false;
    rejecting_writer.reject_write = false;
    UpdateSession after_rejection{rejecting_writer};
    expect(after_rejection.begin(4U) == UpdateSessionResult::success &&
               rejected.abort() == UpdateSessionResult::success &&
               rejected.state() == UpdateSessionState::idle && !rejecting_writer.aborted &&
               after_rejection.write(0U, "good") == UpdateSessionResult::success &&
               after_rejection.finish() == UpdateSessionResult::success &&
               after_rejection.activate() == UpdateSessionResult::success,
        "a failed session cannot abort the next writer lease", failures);

    FakeWriter no_target;
    no_target.usable = false;
    UpdateSession unavailable{no_target};
    expect(unavailable.begin(4U) == UpdateSessionResult::unavailable,
        "an unavailable native writer fails closed", failures);

    FakeWriter bounded;
    UpdateSession size_checked{bounded};
    expect(size_checked.begin(0U) == UpdateSessionResult::invalid_size &&
               size_checked.begin(17U) == UpdateSessionResult::invalid_size && !bounded.began,
        "zero and oversized images are rejected before writer access", failures);

    FakeWriter failed_finish;
    failed_finish.reject_finish = true;
    UpdateSession invalid_image{failed_finish};
    expect(invalid_image.begin(4U) == UpdateSessionResult::success &&
               invalid_image.write(0U, "data") == UpdateSessionResult::success &&
               invalid_image.finish() == UpdateSessionResult::image_rejected &&
               failed_finish.aborted && !failed_finish.activated,
        "whole-image validation failure leaves the image unselected", failures);

    if (failures == 0) {
        std::puts("update session: all checks passed");
    }
    return failures == 0 ? 0 : 1;
}
