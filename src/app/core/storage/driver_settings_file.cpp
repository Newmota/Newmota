/*
  driver_settings_file.cpp - Per-driver fixed-size settings file

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file driver_settings_file.cpp
 * @brief Fixed-size driver records kept in two A/B slots.
 */
#include "core/storage/driver_settings_file.h"
#include "core/storage/filesystem.h"
#include "support/crc32.h"
#include <algorithm>
#include <string_view>

namespace newmota {
namespace {
constexpr std::string_view kDirectory = "/.newmota";
constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kGenerationOffset = kVersionOffset + 1;
constexpr std::size_t kHeaderBytes = kGenerationOffset + 4;
constexpr std::size_t kCrcBytes = 4;
constexpr std::size_t kMaximumFileBytes = kHeaderBytes + kDriverSettingsMaximumPayload + kCrcBytes;
using FileBytes = std::array<std::uint8_t, kMaximumFileBytes>;

std::uint32_t checksum(const std::uint8_t* bytes, std::size_t size) {
    return support::crc32_finalize(support::crc32_update(support::kCrc32InitialState, bytes, size));
}

std::uint32_t read_u32(const std::uint8_t* bytes) {
    std::uint32_t value{};
    for (std::size_t i = 0; i < 4; ++i)
        value |= static_cast<std::uint32_t>(bytes[i]) << (8U * i);
    return value;
}

void write_u32(std::uint8_t* bytes, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i)
        bytes[i] = static_cast<std::uint8_t>(value >> (8U * i));
}

bool sized(const DriverSettingsSpec& spec) {
    return spec.payload_size && spec.payload_size <= kDriverSettingsMaximumPayload;
}

std::size_t file_bytes(const DriverSettingsSpec& spec) {
    return kHeaderBytes + spec.payload_size + kCrcBytes;
}

/** @brief Validates driver records and copies the newest payload to the caller. */
class PayloadReader final : public RecordSlotReader {
  public:
    PayloadReader(const DriverSettingsSpec& spec, std::uint8_t* newest)
        : spec_(spec), newest_(newest) {}

    bool check(const std::uint8_t, const std::uint8_t* const bytes, const std::size_t size,
        std::uint32_t& generation) override {
        const auto crc_offset = kHeaderBytes + spec_.payload_size;
        if (size != file_bytes(spec_) ||
            !std::equal(spec_.magic.begin(), spec_.magic.end(), bytes) ||
            bytes[kVersionOffset] != spec_.version ||
            read_u32(bytes + crc_offset) != checksum(bytes, crc_offset) ||
            (spec_.valid && !spec_.valid(bytes + kHeaderBytes)))
            return false;
        generation = read_u32(bytes + kGenerationOffset);
        return true;
    }

    void keep(const std::uint8_t, const std::uint8_t* const bytes, const std::size_t) override {
        if (newest_)
            std::copy_n(bytes + kHeaderBytes, spec_.payload_size, newest_);
    }

  private:
    const DriverSettingsSpec& spec_;
    std::uint8_t* newest_;
};

/** @brief Reads both slots, copying the newest valid payload into @p newest when it is set. */
RecordSlotsLoad read_slots(const DriverSettingsSpec& spec, FileBytes& bytes, std::uint8_t* newest) {
    PayloadReader reader(spec, newest);
    return spec.slots.load(bytes.data(), bytes.size(), reader);
}

/** @brief Writes @p payload after the newest record that @p stored found, reusing @p bytes. */
bool write_payload(const DriverSettingsSpec& spec, const RecordSlotsLoad& stored, FileBytes& bytes,
    const std::uint8_t* payload) {
    // A slot that could not be read may hold the newest record; never write over it blindly.
    if ((spec.valid && !spec.valid(payload)) || !stored.readable())
        return false;
    bytes = {};
    std::copy(spec.magic.begin(), spec.magic.end(), bytes.begin());
    bytes[kVersionOffset] = spec.version;
    write_u32(bytes.data() + kGenerationOffset, next_generation(stored.generation));
    std::copy_n(payload, spec.payload_size, bytes.begin() + kHeaderBytes);
    const auto crc_offset = kHeaderBytes + spec.payload_size;
    write_u32(bytes.data() + crc_offset, checksum(bytes.data(), crc_offset));
    namespace fs = filesystem;
    return fs::ensure_directory(kDirectory) == fs::MutationResult::succeeded &&
           spec.slots.write(stored.slot, bytes.data(), file_bytes(spec)) ==
               fs::MutationResult::succeeded;
}
} // namespace

bool save_driver_settings(const DriverSettingsSpec& spec, const std::uint8_t* payload) {
    if (!sized(spec))
        return false;
    FileBytes bytes{};
    const auto stored = read_slots(spec, bytes, nullptr);
    return write_payload(spec, stored, bytes, payload);
}

DriverSettingsState load_driver_settings(const DriverSettingsSpec& spec, std::uint8_t* payload) {
    if (!sized(spec))
        return DriverSettingsState::failed;
    FileBytes bytes{};
    const auto stored = read_slots(spec, bytes, payload);
    if (stored.loaded())
        return stored.clean() ? DriverSettingsState::loaded : DriverSettingsState::recovered;
    // Missing, or no valid slot: the incoming defaults become the record.
    if (!write_payload(spec, stored, bytes, payload))
        return DriverSettingsState::failed;
    return stored.states[0] == RecordSlotState::absent &&
                   stored.states[1] == RecordSlotState::absent
               ? DriverSettingsState::created
               : DriverSettingsState::defaulted;
}
} // namespace newmota
