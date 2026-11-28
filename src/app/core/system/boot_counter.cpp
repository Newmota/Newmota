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
#include "core/storage/record_slots.h"
#include "newmota/log.h"
#include "support/crc32.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace newmota {
namespace {

constexpr char kSettingsDirectory[] = "/.newmota";
/**
 * The two boot-count slots. The count is the record's generation: a boot writes the highest
 * stored count plus one into the other slot, and a reset removes both slots before it writes 0.
 */
constexpr RecordSlots kSlots{"/.newmota/boot.a", "/.newmota/boot.b"};
constexpr std::array<std::uint8_t, 4> kMagic{'N', 'M', 'B', 'C'};
constexpr std::uint16_t kFormatVersion = 1U;
constexpr std::size_t kCrcOffset = 12U;
constexpr std::size_t kRecordSize = 16U;

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
[[nodiscard]] bool decode_record(const std::uint8_t* const record, std::uint32_t& count) {
    for (std::size_t index = 0; index < kMagic.size(); ++index) {
        if (record[index] != kMagic[index]) {
            return false;
        }
    }
    if (read_u16(record + 4U) != kFormatVersion || read_u16(record + 6U) != 0U) {
        return false;
    }
    const auto raw = support::crc32_update(support::kCrc32InitialState, record, kCrcOffset);
    if (support::crc32_finalize(raw) != read_u32(record + kCrcOffset)) {
        return false;
    }
    count = read_u32(record + 8U);
    return true;
}

/** @brief Accepts valid boot-count records; the count is the generation. */
class BootRecordReader final : public RecordSlotReader {
  public:
    bool check(const std::uint8_t, const std::uint8_t* const bytes, const std::size_t size,
        std::uint32_t& generation) override {
        return size == kRecordSize && decode_record(bytes, generation);
    }

    void keep(const std::uint8_t, const std::uint8_t* const, const std::size_t) override {}
};

/** @brief Reads both slots; the result's generation is the newest stored count. */
[[nodiscard]] RecordSlotsLoad read_records() {
    std::array<std::uint8_t, kRecordSize> buffer{};
    BootRecordReader reader;
    return kSlots.load(buffer.data(), buffer.size(), reader);
}

/** @brief Writes @p count into the slot not holding @p loaded. */
[[nodiscard]] bool write_record(const std::uint8_t loaded, const std::uint32_t count) {
    const auto record = encode_record(count);
    return kSlots.write(loaded, record.data(), record.size()) ==
           filesystem::MutationResult::succeeded;
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

    const auto stored = read_records();
    // Never count on from a guess: a slot that could not be read may hold the highest count, and
    // an invalid record without a valid one leaves no count at all.
    if (!stored.readable() || (!stored.loaded() && stored.any(RecordSlotState::invalid))) {
        return false;
    }
    if (!increment && stored.loaded()) {
        count_ = stored.generation;
        available_ = true;
        return true;
    }
    const auto next_count = stored.generation == UINT32_MAX ? UINT32_MAX : stored.generation + 1U;
    if (!write_record(stored.slot, next_count)) {
        return false;
    }
    count_ = next_count;
    available_ = true;
    return true;
}

bool BootCounter::reset() {
    if (!filesystem::is_mounted() ||
        filesystem::ensure_directory(kSettingsDirectory) != filesystem::MutationResult::succeeded) {
        return false;
    }
    // A stored count would win over a new 0, so both slots go first, the older one before the
    // newest; a failure in between keeps the newest count.
    if (kSlots.remove_both(read_records().slot) != filesystem::MutationResult::succeeded ||
        !write_record(kNoRecordSlot, 0U)) {
        return false;
    }
    count_ = 0U;
    available_ = true;
    return true;
}

} // namespace newmota
