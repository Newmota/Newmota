/*
  boot_counter.cpp - persistent application boot counter for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "core/system/boot_counter.h"

#include "core/storage/filesystem.h"
#include "newmota/log.h"
#include "support/crc32.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace newmota {
namespace {

constexpr char kSettingsDirectory[] = "/.newmota";
constexpr char kActivePath[] = "/.newmota/boot.bin";
constexpr char kPreviousPath[] = "/.newmota/boot.prev";
constexpr char kTemporaryPath[] = "/.newmota/boot.tmp";
constexpr std::array<std::uint8_t, 4> kMagic{'N', 'M', 'B', 'C'};
constexpr std::uint16_t kFormatVersion = 1U;
constexpr std::size_t kCrcOffset = 12U;
constexpr std::size_t kRecordSize = 16U;

enum class RecordState : std::uint8_t {
    absent,
    valid,
    invalid,
    failed,
};

/** @brief Writes one little-endian 16-bit field into a record. */
void write_u16(std::uint8_t* const destination, const std::uint16_t value) {
    destination[0] = static_cast<std::uint8_t>(value);
    destination[1] = static_cast<std::uint8_t>(value >> 8U);
}

/** @brief Writes one little-endian 32-bit field into a record. */
void write_u32(std::uint8_t* const destination, const std::uint32_t value) {
    destination[0] = static_cast<std::uint8_t>(value);
    destination[1] = static_cast<std::uint8_t>(value >> 8U);
    destination[2] = static_cast<std::uint8_t>(value >> 16U);
    destination[3] = static_cast<std::uint8_t>(value >> 24U);
}

/** @brief Reads one little-endian 16-bit field from a record. */
[[nodiscard]] std::uint16_t read_u16(const std::uint8_t* const source) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(source[0]) | static_cast<std::uint16_t>(source[1]) << 8U);
}

/** @brief Reads one little-endian 32-bit field from a record. */
[[nodiscard]] std::uint32_t read_u32(const std::uint8_t* const source) {
    return static_cast<std::uint32_t>(source[0]) | static_cast<std::uint32_t>(source[1]) << 8U |
           static_cast<std::uint32_t>(source[2]) << 16U |
           static_cast<std::uint32_t>(source[3]) << 24U;
}

/** @brief Encodes one CRC-protected boot-count record. */
[[nodiscard]] std::array<std::uint8_t, kRecordSize> encode_record(const std::uint32_t count) {
    std::array<std::uint8_t, kRecordSize> record{};
    for (std::size_t index = 0; index < kMagic.size(); ++index) {
        record[index] = kMagic[index];
    }
    write_u16(record.data() + 4U, kFormatVersion);
    write_u16(record.data() + 6U, 0U);
    write_u32(record.data() + 8U, count);

    const auto raw = support::crc32_update(support::kCrc32InitialState, record.data(), kCrcOffset);
    write_u32(record.data() + kCrcOffset, support::crc32_finalize(raw));
    return record;
}

/** @brief Validates one record and extracts its boot count. */
[[nodiscard]] bool decode_record(
    const std::array<std::uint8_t, kRecordSize>& record, std::uint32_t& count) {
    for (std::size_t index = 0; index < kMagic.size(); ++index) {
        if (record[index] != kMagic[index]) {
            return false;
        }
    }
    if (read_u16(record.data() + 4U) != kFormatVersion || read_u16(record.data() + 6U) != 0U) {
        return false;
    }
    const auto raw = support::crc32_update(support::kCrc32InitialState, record.data(), kCrcOffset);
    if (support::crc32_finalize(raw) != read_u32(record.data() + kCrcOffset)) {
        return false;
    }
    count = read_u32(record.data() + 8U);
    return true;
}

/** @brief Reads one optional record from the filesystem. */
[[nodiscard]] RecordState read_record(const char* const path, std::uint32_t& count) {
    std::array<std::uint8_t, kRecordSize> record{};
    std::size_t size = 0U;
    const auto result = filesystem::read_file(path, record.data(), record.size(), size);
    if (result == filesystem::ReadResult::not_found) {
        return RecordState::absent;
    }
    if (result != filesystem::ReadResult::succeeded) {
        return RecordState::failed;
    }
    if (size != record.size() || !decode_record(record, count)) {
        return RecordState::invalid;
    }
    return RecordState::valid;
}

/** @brief Writes and reads back one temporary record. */
[[nodiscard]] bool write_temporary_record(const std::uint32_t count) {
    const auto record = encode_record(count);
    if (filesystem::write_file(kTemporaryPath, record.data(), record.size()) !=
        filesystem::MutationResult::succeeded) {
        return false;
    }
    std::uint32_t verified_count = 0U;
    return read_record(kTemporaryPath, verified_count) == RecordState::valid &&
           verified_count == count;
}

/**
 * @brief Commits a new count while retaining one recoverable previous record.
 * @param count Value to make active.
 * @param clear_history Whether an explicit reset should discard old counts.
 */
[[nodiscard]] bool commit_record(
    const std::uint32_t count, const bool clear_history, const RecordState active_state) {
    if (filesystem::remove_if_exists(kTemporaryPath) != filesystem::MutationResult::succeeded) {
        return false;
    }
    if (!write_temporary_record(count)) {
        (void)filesystem::remove_if_exists(kTemporaryPath);
        return false;
    }

    bool moved_active = false;
    if (clear_history) {
        if (filesystem::remove_if_exists(kPreviousPath) != filesystem::MutationResult::succeeded ||
            filesystem::remove_if_exists(kActivePath) != filesystem::MutationResult::succeeded) {
            (void)filesystem::remove_if_exists(kTemporaryPath);
            return false;
        }
    } else if (active_state == RecordState::valid) {
        if (filesystem::remove_if_exists(kPreviousPath) != filesystem::MutationResult::succeeded ||
            filesystem::rename(kActivePath, kPreviousPath) !=
                filesystem::MutationResult::succeeded) {
            (void)filesystem::remove_if_exists(kTemporaryPath);
            return false;
        }
        moved_active = true;
    } else if (active_state == RecordState::invalid &&
               filesystem::remove_if_exists(kActivePath) != filesystem::MutationResult::succeeded) {
        (void)filesystem::remove_if_exists(kTemporaryPath);
        return false;
    }

    if (filesystem::rename(kTemporaryPath, kActivePath) == filesystem::MutationResult::succeeded) {
        return true;
    }

    (void)filesystem::remove_if_exists(kTemporaryPath);
    if (moved_active) {
        (void)filesystem::remove_if_exists(kActivePath);
        (void)filesystem::rename(kPreviousPath, kActivePath);
    }
    return false;
}

} // namespace

bool BootCounter::initialize(const bool increment) {
    count_ = 0U;
    available_ = false;
    if (!filesystem::is_mounted()) {
        return false;
    }
    const auto directory_result = filesystem::ensure_directory(kSettingsDirectory);
    if (directory_result != filesystem::MutationResult::succeeded) {
        return false;
    }

    std::uint32_t active_count = 0U;
    std::uint32_t previous_count = 0U;
    const auto active_state = read_record(kActivePath, active_count);
    const auto previous_state = read_record(kPreviousPath, previous_count);
    if (active_state == RecordState::failed || previous_state == RecordState::failed ||
        (active_state == RecordState::invalid && previous_state != RecordState::valid) ||
        (previous_state == RecordState::invalid && active_state != RecordState::valid)) {
        return false;
    }

    std::uint32_t previous_highest = 0U;
    if (active_state == RecordState::valid) {
        previous_highest = active_count;
    }
    if (previous_state == RecordState::valid &&
        (active_state != RecordState::valid || previous_count > previous_highest)) {
        previous_highest = previous_count;
    }
    if (!increment &&
        (active_state == RecordState::valid || previous_state == RecordState::valid)) {
        count_ = previous_highest;
        available_ = true;
        return true;
    }
    const auto next_count = previous_highest == UINT32_MAX ? UINT32_MAX : previous_highest + 1U;
    if (!commit_record(next_count, false, active_state)) {
        return false;
    }
    count_ = next_count;
    available_ = true;
    return true;
}

bool BootCounter::reset() {
    if (!filesystem::is_mounted() ||
        filesystem::ensure_directory(kSettingsDirectory) != filesystem::MutationResult::succeeded ||
        !commit_record(0U, true, RecordState::absent)) {
        return false;
    }
    count_ = 0U;
    available_ = true;
    return true;
}

} // namespace newmota
