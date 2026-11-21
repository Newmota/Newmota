/*
  driver_settings_file.cpp - Per-driver fixed-size settings file

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file driver_settings_file.cpp
 * @brief Verified temporary-then-rename replacement with one recovery generation.
 */
#include "core/storage/driver_settings_file.h"
#include "core/storage/filesystem.h"
#include "support/crc32.h"
#include <algorithm>
#include <string_view>

namespace newmota {
namespace {
constexpr std::string_view kDirectory = "/.newmota";
constexpr std::size_t kHeaderBytes = 5;
constexpr std::size_t kCrcBytes = 4;
constexpr std::size_t kMaximumFileBytes = kHeaderBytes + kDriverSettingsMaximumPayload + kCrcBytes;

enum class Read : std::uint8_t { valid, missing, unusable };

std::uint32_t checksum(const std::uint8_t* bytes, std::size_t size) {
    return support::crc32_finalize(support::crc32_update(support::kCrc32InitialState, bytes, size));
}

bool sized(const DriverSettingsSpec& spec) {
    return spec.payload_size && spec.payload_size <= kDriverSettingsMaximumPayload;
}

std::size_t file_bytes(const DriverSettingsSpec& spec) {
    return kHeaderBytes + spec.payload_size + kCrcBytes;
}

Read read_payload(const DriverSettingsSpec& spec, const char* path, std::uint8_t* payload) {
    std::array<std::uint8_t, kMaximumFileBytes> bytes{};
    std::size_t size{};
    const auto result = filesystem::read_file(path, bytes.data(), bytes.size(), size);
    if (result == filesystem::ReadResult::not_found)
        return Read::missing;
    const auto crc_offset = kHeaderBytes + spec.payload_size;
    if (result != filesystem::ReadResult::succeeded || size != file_bytes(spec) ||
        !std::equal(spec.magic.begin(), spec.magic.end(), bytes.begin()) ||
        bytes[spec.magic.size()] != spec.version)
        return Read::unusable;
    std::uint32_t stored{};
    for (std::size_t i = 0; i < kCrcBytes; ++i)
        stored |= static_cast<std::uint32_t>(bytes[crc_offset + i]) << (8U * i);
    if (stored != checksum(bytes.data(), crc_offset) ||
        (spec.valid && !spec.valid(bytes.data() + kHeaderBytes)))
        return Read::unusable;
    std::copy_n(bytes.begin() + kHeaderBytes, spec.payload_size, payload);
    return Read::valid;
}
} // namespace

bool save_driver_settings(const DriverSettingsSpec& spec, const std::uint8_t* payload) {
    if (!sized(spec) || (spec.valid && !spec.valid(payload)))
        return false;
    std::array<std::uint8_t, kMaximumFileBytes> bytes{};
    std::copy(spec.magic.begin(), spec.magic.end(), bytes.begin());
    bytes[spec.magic.size()] = spec.version;
    std::copy_n(payload, spec.payload_size, bytes.begin() + kHeaderBytes);
    const auto crc_offset = kHeaderBytes + spec.payload_size;
    const auto crc = checksum(bytes.data(), crc_offset);
    for (std::size_t i = 0; i < kCrcBytes; ++i)
        bytes[crc_offset + i] = static_cast<std::uint8_t>(crc >> (8U * i));
    const auto size = file_bytes(spec);
    namespace fs = filesystem;
    if (fs::ensure_directory(kDirectory) != fs::MutationResult::succeeded ||
        fs::remove_if_exists(spec.temporary) != fs::MutationResult::succeeded ||
        fs::write_file(spec.temporary, bytes.data(), size) != fs::MutationResult::succeeded)
        return false;
    std::array<std::uint8_t, kMaximumFileBytes> verified{};
    std::size_t read_size{};
    if (fs::read_file(spec.temporary, verified.data(), verified.size(), read_size) !=
            fs::ReadResult::succeeded ||
        read_size != size || !std::equal(bytes.begin(), bytes.begin() + size, verified.begin()))
        return false;
    std::array<std::uint8_t, kDriverSettingsMaximumPayload> current{};
    const auto active = read_payload(spec, spec.path, current.data());
    if (active == Read::valid) {
        if (fs::remove_if_exists(spec.previous) != fs::MutationResult::succeeded ||
            fs::rename(spec.path, spec.previous) != fs::MutationResult::succeeded)
            return false;
    } else if (active == Read::unusable &&
               fs::remove_if_exists(spec.path) != fs::MutationResult::succeeded)
        return false;
    if (fs::rename(spec.temporary, spec.path) == fs::MutationResult::succeeded)
        return true;
    if (active == Read::valid)
        (void)fs::rename(spec.previous, spec.path);
    return false;
}

DriverSettingsState load_driver_settings(const DriverSettingsSpec& spec, std::uint8_t* payload) {
    if (!sized(spec))
        return DriverSettingsState::failed;
    std::array<std::uint8_t, kDriverSettingsMaximumPayload> stored{};
    const auto active = read_payload(spec, spec.path, stored.data());
    if (active == Read::valid) {
        std::copy_n(stored.begin(), spec.payload_size, payload);
        return DriverSettingsState::loaded;
    }
    if (active == Read::unusable &&
        read_payload(spec, spec.previous, stored.data()) == Read::valid) {
        std::copy_n(stored.begin(), spec.payload_size, payload);
        return DriverSettingsState::recovered;
    }
    // Missing, or corrupt without a recovery generation: the incoming defaults become the file.
    if (!save_driver_settings(spec, payload))
        return DriverSettingsState::failed;
    return active == Read::missing ? DriverSettingsState::created : DriverSettingsState::defaulted;
}
} // namespace newmota
