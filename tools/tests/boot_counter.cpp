/*
  boot_counter.cpp - host tests for Newmota's persistent boot counter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "core/storage/filesystem.h"
#include "core/system/boot_counter.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace {

constexpr std::size_t kRecordCapacity = 32U;

struct FileSlot final {
    std::array<std::uint8_t, kRecordCapacity> bytes{};
    std::size_t size{};
    bool present{};
};

std::array<FileSlot, 2> files{};

/** @brief Finds one boot-counter fixture path. */
[[nodiscard]] FileSlot* file_for(const std::string_view path) {
    if (path == "/.newmota/boot.a")
        return &files[0];
    if (path == "/.newmota/boot.b")
        return &files[1];
    return nullptr;
}

/** @brief Returns the count stored in a slot, or UINT32_MAX for a missing slot. */
[[nodiscard]] std::uint32_t count_of(const FileSlot& file) {
    if (!file.present || file.size < 12U)
        return UINT32_MAX;
    std::uint32_t value = 0U;
    for (unsigned byte = 0U; byte < 4U; ++byte)
        value |= static_cast<std::uint32_t>(file.bytes[8U + byte]) << (8U * byte);
    return value;
}

/** @brief Clears the fixed filesystem fixture. */
void clear_files() {
    files = {};
}

/** @brief Records one failed expectation. */
void expect(const bool condition, const char* const message, int& failures) {
    if (!condition) {
        std::fprintf(stderr, "boot counter: %s\n", message);
        ++failures;
    }
}

} // namespace

namespace newmota::filesystem {

bool is_mounted(Volume) {
    return true;
}

MutationResult ensure_directory(const std::string_view path) {
    return path == "/.newmota" ? MutationResult::succeeded : MutationResult::invalid_path;
}

ReadResult read_file(
    const std::string_view path, void* const data, const std::size_t capacity, std::size_t& size) {
    size = 0U;
    const FileSlot* const file = file_for(path);
    if (file == nullptr)
        return ReadResult::invalid_path;
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
    FileSlot* const file = file_for(path);
    if (file == nullptr || size > file->bytes.size()) {
        return MutationResult::invalid_path;
    }
    std::memcpy(file->bytes.data(), data, size);
    file->size = size;
    file->present = true;
    return MutationResult::succeeded;
}

MutationResult remove_if_exists(const std::string_view path) {
    FileSlot* const file = file_for(path);
    if (file == nullptr)
        return MutationResult::invalid_path;
    *file = {};
    return MutationResult::succeeded;
}

} // namespace newmota::filesystem

int main() {
    using namespace newmota;
    int failures = 0;

    clear_files();
    BootCounter first;
    expect(first.initialize() && first.status().available && first.status().count == 1U &&
               count_of(files[0]) == 1U && !files[1].present,
        "first boot creates count one in slot A", failures);

    BootCounter second;
    expect(second.initialize() && second.status().count == 2U && count_of(files[0]) == 1U &&
               count_of(files[1]) == 2U,
        "second boot writes the incremented count into slot B", failures);

    BootCounter third;
    expect(third.initialize() && third.status().count == 3U && count_of(files[0]) == 3U &&
               count_of(files[1]) == 2U,
        "third boot replaces the older slot A and keeps the previous count in B", failures);

    BootCounter peek;
    expect(peek.initialize(false) && peek.status().count == 3U && count_of(files[0]) == 3U &&
               count_of(files[1]) == 2U,
        "a load without increment reads the newest count and writes nothing", failures);

    expect(third.reset() && third.status().available && third.status().count == 0U &&
               count_of(files[0]) == 0U && !files[1].present,
        "explicit reset removes both slots and persists zero in slot A", failures);
    BootCounter after_reset;
    expect(after_reset.initialize() && after_reset.status().count == 1U && count_of(files[1]) == 1U,
        "next boot starts at one after reset", failures);

    clear_files();
    BootCounter newer_corrupt;
    expect(newer_corrupt.initialize() && newer_corrupt.initialize(),
        "fallback fixture has two records", failures);
    files[1].bytes[0] ^= 0x01U;
    BootCounter fell_back;
    expect(fell_back.initialize() && fell_back.status().count == 2U && count_of(files[1]) == 2U,
        "a corrupt newer slot falls back to the older count and is replaced", failures);

    clear_files();
    BootCounter empty_slot;
    expect(empty_slot.initialize(), "0-byte fixture has an initial record", failures);
    files[1].present = true;
    files[1].size = 0U;
    BootCounter after_empty;
    expect(after_empty.initialize() && after_empty.status().count == 2U && count_of(files[1]) == 2U,
        "a 0-byte slot counts as absent", failures);

    clear_files();
    BootCounter corrupted;
    expect(corrupted.initialize(), "corruption fixture has an initial record", failures);
    files[0].bytes[0] ^= 0x01U;
    BootCounter rejected;
    expect(!rejected.initialize() && !rejected.status().available,
        "corrupt sole record is rejected without inventing a count", failures);

    if (failures == 0) {
        std::puts("Boot counter tests passed");
    }
    return failures == 0 ? 0 : 1;
}
