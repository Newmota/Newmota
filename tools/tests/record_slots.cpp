/*
  record_slots.cpp - host tests for Newmota's A/B record slots

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "core/storage/record_slots.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace {

using newmota::RecordSlots;
using newmota::RecordSlotsLoad;
using newmota::RecordSlotState;
using newmota::filesystem::MutationResult;
using newmota::filesystem::ReadResult;

/** Test record: generation (4, little endian), value (1), check byte (1). */
constexpr std::size_t kRecordSize = 6U;

struct FileSlot final {
    std::array<std::uint8_t, 16> bytes{};
    std::size_t size{};
    bool present{};
    ReadResult read_failure{ReadResult::succeeded};
    bool write_failure{};
    bool remove_failure{};
    unsigned writes{};
};

std::array<FileSlot, 2> files{};
std::array<std::string_view, 4> removal_order{};
std::size_t removals{};

constexpr RecordSlots kSlots{"/t/a", "/t/b"};

[[nodiscard]] FileSlot* file_for(const std::string_view path) {
    if (path == "/t/a")
        return &files[0];
    if (path == "/t/b")
        return &files[1];
    return nullptr;
}

void clear_files() {
    files = {};
    removal_order = {};
    removals = 0U;
}

[[nodiscard]] std::uint8_t check_byte(const std::uint8_t* const bytes) {
    std::uint8_t value = 0x5AU;
    for (std::size_t index = 0U; index + 1U < kRecordSize; ++index)
        value = static_cast<std::uint8_t>(value ^ bytes[index]);
    return value;
}

[[nodiscard]] std::array<std::uint8_t, kRecordSize> record(
    const std::uint32_t generation, const std::uint8_t value) {
    std::array<std::uint8_t, kRecordSize> bytes{};
    for (unsigned byte = 0U; byte < 4U; ++byte)
        bytes[byte] = static_cast<std::uint8_t>(generation >> (8U * byte));
    bytes[4] = value;
    bytes[5] = check_byte(bytes.data());
    return bytes;
}

void store(const std::uint8_t slot, const std::uint32_t generation, const std::uint8_t value) {
    const auto bytes = record(generation, value);
    std::memcpy(files[slot].bytes.data(), bytes.data(), bytes.size());
    files[slot].size = bytes.size();
    files[slot].present = true;
}

/** @brief Validates test records and keeps the newest value, counting the calls. */
class TestReader final : public newmota::RecordSlotReader {
  public:
    bool check(const std::uint8_t slot, const std::uint8_t* const bytes, const std::size_t size,
        std::uint32_t& generation) override {
        ++checks[slot];
        if (size != kRecordSize || bytes[5] != check_byte(bytes))
            return false;
        generation = static_cast<std::uint32_t>(bytes[0]) |
                     static_cast<std::uint32_t>(bytes[1]) << 8U |
                     static_cast<std::uint32_t>(bytes[2]) << 16U |
                     static_cast<std::uint32_t>(bytes[3]) << 24U;
        return true;
    }

    void keep(
        const std::uint8_t slot, const std::uint8_t* const bytes, const std::size_t) override {
        kept_slot = slot;
        value = bytes[4];
    }

    std::array<unsigned, 2> checks{};
    std::uint8_t kept_slot{newmota::kNoRecordSlot};
    std::uint8_t value{};
};

struct Loaded final {
    RecordSlotsLoad result;
    std::uint8_t value;
};

[[nodiscard]] Loaded load() {
    std::array<std::uint8_t, 8> buffer{};
    TestReader reader;
    const auto result = kSlots.load(buffer.data(), buffer.size(), reader);
    return {result, reader.value};
}

/** @brief Saves @p value after the newest loaded record, as a device save does. */
[[nodiscard]] bool save(const std::uint8_t value) {
    const auto stored = load();
    if (!stored.result.readable())
        return false;
    const auto bytes = record(newmota::next_generation(stored.result.generation), value);
    return kSlots.write(stored.result.slot, bytes.data(), bytes.size()) ==
           MutationResult::succeeded;
}

void expect(const bool condition, const char* const message, int& failures) {
    if (!condition) {
        std::fprintf(stderr, "record slots: %s\n", message);
        ++failures;
    }
}

} // namespace

namespace newmota::filesystem {

ReadResult read_file(
    const std::string_view path, void* const data, const std::size_t capacity, std::size_t& size) {
    size = 0U;
    const auto* const file = file_for(path);
    if (file == nullptr)
        return ReadResult::invalid_path;
    if (file->read_failure != ReadResult::succeeded)
        return file->read_failure;
    if (!file->present)
        return ReadResult::not_found;
    if (file->size > capacity)
        return ReadResult::too_large;
    std::memcpy(data, file->bytes.data(), file->size);
    size = file->size;
    return ReadResult::succeeded;
}

MutationResult write_file(
    const std::string_view path, const void* const data, const std::size_t size) {
    auto* const file = file_for(path);
    if (file == nullptr || size > file->bytes.size())
        return MutationResult::invalid_path;
    if (file->write_failure)
        return MutationResult::failed;
    std::memcpy(file->bytes.data(), data, size);
    file->size = size;
    file->present = true;
    ++file->writes;
    return MutationResult::succeeded;
}

MutationResult remove_if_exists(const std::string_view path) {
    auto* const file = file_for(path);
    if (file == nullptr)
        return MutationResult::invalid_path;
    if (file->remove_failure)
        return MutationResult::failed;
    if (removals < removal_order.size())
        removal_order[removals++] = path;
    *file = {};
    return MutationResult::succeeded;
}

} // namespace newmota::filesystem

int main() {
    using newmota::kNoRecordSlot;
    using newmota::newer_generation;
    using newmota::next_generation;
    int failures = 0;

    expect(newer_generation(2U, 1U) && !newer_generation(1U, 2U) && !newer_generation(5U, 5U) &&
               newer_generation(1U, UINT32_MAX) && !newer_generation(UINT32_MAX, 1U) &&
               newer_generation(0x80000000U, 1U) && !newer_generation(0x80000001U, 1U),
        "generations compare in serial order", failures);
    expect(next_generation(1U) == 2U && next_generation(UINT32_MAX) == 1U,
        "the generation after UINT32_MAX is 1", failures);

    clear_files();
    auto empty = load();
    expect(!empty.result.loaded() && empty.result.slot == kNoRecordSlot &&
               empty.result.generation == 0U && empty.result.clean() && empty.result.readable() &&
               empty.result.states[0] == RecordSlotState::absent &&
               empty.result.states[1] == RecordSlotState::absent,
        "two missing slots load nothing", failures);

    expect(save(10U) && files[0].writes == 1U && files[1].writes == 0U && save(11U) &&
               files[1].writes == 1U && save(12U) && files[0].writes == 2U && save(13U) &&
               files[1].writes == 2U,
        "saves alternate between the slots, starting with A", failures);
    auto alternating = load();
    expect(alternating.result.slot == 1U && alternating.result.generation == 4U &&
               alternating.value == 13U && alternating.result.clean(),
        "the newest of two valid slots wins", failures);

    clear_files();
    store(0U, 9U, 1U);
    store(1U, 8U, 2U);
    auto in_a = load();
    store(0U, 8U, 1U);
    store(1U, 9U, 2U);
    auto in_b = load();
    store(0U, 9U, 1U);
    store(1U, 9U, 2U);
    auto equal = load();
    expect(in_a.result.slot == 0U && in_a.value == 1U && in_b.result.slot == 1U &&
               in_b.value == 2U && equal.result.slot == 0U && equal.value == 1U,
        "the newest slot wins in either position and equal generations pick A", failures);

    clear_files();
    store(0U, 4U, 1U);
    store(1U, 5U, 2U);
    files[1].bytes[4] ^= 0x01U;
    auto fallback = load();
    expect(fallback.result.slot == 0U && fallback.value == 1U && fallback.result.generation == 4U &&
               fallback.result.states[1] == RecordSlotState::invalid && !fallback.result.clean() &&
               fallback.result.readable(),
        "a corrupt newer slot falls back to the older one", failures);
    expect(save(3U) && files[1].writes == 1U && load().result.slot == 1U &&
               load().result.generation == 5U && load().value == 3U,
        "the next save replaces the corrupt slot and keeps the loaded one", failures);

    clear_files();
    store(0U, 6U, 1U);
    files[1].present = true;
    files[1].size = 0U;
    auto zero = load();
    TestReader zero_reader;
    std::array<std::uint8_t, 8> zero_buffer{};
    (void)kSlots.load(zero_buffer.data(), zero_buffer.size(), zero_reader);
    expect(zero.result.slot == 0U && zero.result.states[1] == RecordSlotState::absent &&
               zero.result.clean() && zero_reader.checks[1] == 0U,
        "a 0-byte slot counts as absent and is never checked", failures);
    expect(save(2U) && files[1].size == kRecordSize && load().result.slot == 1U,
        "a 0-byte slot is written next", failures);

    clear_files();
    store(0U, UINT32_MAX - 1U, 1U);
    store(1U, UINT32_MAX, 2U);
    expect(load().result.slot == 1U && save(3U) && files[0].writes == 1U,
        "a save at UINT32_MAX wraps", failures);
    auto wrapped = load();
    expect(wrapped.result.slot == 0U && wrapped.result.generation == 1U && wrapped.value == 3U,
        "generation 1 is newer than UINT32_MAX", failures);
    expect(save(4U) && files[1].writes == 1U && load().result.slot == 1U &&
               load().result.generation == 2U && load().value == 4U,
        "after the wrap the next save replaces the slot holding UINT32_MAX", failures);

    clear_files();
    store(0U, 2U, 1U);
    files[1].read_failure = ReadResult::failed;
    auto failed = load();
    expect(failed.result.slot == 0U && failed.result.states[1] == RecordSlotState::failed &&
               !failed.result.readable() && !failed.result.clean() && !save(5U) &&
               files[0].writes == 0U && files[1].writes == 0U,
        "an unreadable slot is reported and a save refuses to write blindly", failures);
    files[1].read_failure = ReadResult::unavailable;
    expect(load().result.states[1] == RecordSlotState::unavailable,
        "an unmounted filesystem reads as unavailable", failures);
    files[1] = {};
    files[1].present = true;
    files[1].size = 12U;
    std::array<std::uint8_t, kRecordSize> small{};
    TestReader small_reader;
    const auto too_large = kSlots.load(small.data(), small.size(), small_reader);
    expect(too_large.slot == 0U && too_large.states[1] == RecordSlotState::invalid &&
               small_reader.checks[1] == 0U,
        "a slot larger than the buffer is invalid", failures);

    clear_files();
    store(0U, 7U, 1U);
    store(1U, 6U, 2U);
    files[1].write_failure = true;
    const auto before = files[0];
    expect(!save(9U) && std::memcmp(files[0].bytes.data(), before.bytes.data(), kRecordSize) == 0 &&
               load().result.slot == 0U && load().value == 1U,
        "a failed write leaves the loaded slot unchanged", failures);

    clear_files();
    store(0U, 3U, 1U);
    store(1U, 4U, 2U);
    expect(kSlots.remove_both(1U) == MutationResult::succeeded && !files[0].present &&
               !files[1].present && removals == 2U && removal_order[0] == "/t/a" &&
               removal_order[1] == "/t/b",
        "remove_both removes the older slot first, then the newest", failures);
    store(0U, 3U, 1U);
    store(1U, 4U, 2U);
    files[1].remove_failure = true;
    expect(kSlots.remove_both(1U) == MutationResult::failed && !files[0].present &&
               files[1].present && load().result.slot == 1U,
        "a failed removal keeps the newest record", failures);
    files[1].remove_failure = false;
    expect(kSlots.remove_both() == MutationResult::succeeded && !files[0].present &&
               !files[1].present && kSlots.remove(0U) == MutationResult::succeeded,
        "removing missing slots succeeds", failures);

    if (failures == 0) {
        std::puts("Record slots tests passed");
    }
    return failures == 0 ? 0 : 1;
}
