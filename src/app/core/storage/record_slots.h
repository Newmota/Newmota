/*
  record_slots.h - Two-slot (A/B) record files for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file record_slots.h
 * @brief One record kept in two files, A and B, each written with one atomic replacement.
 *
 * A save writes the whole record with one filesystem::write_file into the slot that was not
 * loaded. LittleFS commits a file on sync and validates every program, so the loaded slot stays
 * intact until the new record is complete; a power cut leaves either the old content of the
 * written slot or the complete new record. No temporary file, read-back or rename is needed.
 *
 * Each record carries a generation inside its own checksummed format; the caller's check reads
 * it. A load reads both slots and keeps the valid record with the newest generation in serial
 * order, so the count may wrap. Equal generations pick slot A. A missing or 0-byte slot is
 * absent: LittleFS commits a new file's entry before its data, so a power cut during the first
 * write of a slot leaves it empty.
 */
#pragma once
#include "core/storage/filesystem.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace newmota {

/** @brief Slot index meaning that neither slot holds a valid record. */
inline constexpr std::uint8_t kNoRecordSlot = 2U;

/**
 * @brief Reports whether generation @p candidate was written after @p reference.
 *
 * Serial-number order (RFC 1982) keeps two adjacent generations ordered across the wrap.
 */
[[nodiscard]] constexpr bool newer_generation(
    const std::uint32_t candidate, const std::uint32_t reference) {
    return candidate != reference &&
           static_cast<std::uint32_t>(candidate - reference) < 0x80000000U;
}

static_assert(newer_generation(2U, 1U) && !newer_generation(1U, 2U));
static_assert(newer_generation(1U, UINT32_MAX) && !newer_generation(UINT32_MAX, 1U));
static_assert(!newer_generation(7U, 7U));

/** @brief The generation a save writes after @p generation; it skips 0 when it wraps. */
[[nodiscard]] constexpr std::uint32_t next_generation(const std::uint32_t generation) {
    return generation == UINT32_MAX ? 1U : generation + 1U;
}

/** @brief Physical and logical state of one slot after a load. */
enum class RecordSlotState : std::uint8_t {
    absent,      /**< Missing or 0 bytes. */
    valid,       /**< Read and accepted by the caller's check. */
    invalid,     /**< Read and rejected, or larger than the load buffer. */
    unavailable, /**< The filesystem is not mounted. */
    failed,      /**< A native read failure or an invalid path. */
};

/** @brief Validates and takes the records a load reads. */
class RecordSlotReader {
  public:
    /**
     * @brief Validates the non-empty record read from @p slot.
     * @param generation Set to the record's generation when it is valid.
     * @return true when the record is valid.
     */
    [[nodiscard]] virtual bool check(std::uint8_t slot, const std::uint8_t* bytes,
        std::size_t size, std::uint32_t& generation) = 0;
    /**
     * @brief Takes the record of @p slot, the newest valid one read so far.
     *
     * Called right after check() accepted that slot, while @p bytes still hold its record.
     */
    virtual void keep(std::uint8_t slot, const std::uint8_t* bytes, std::size_t size) = 0;

  protected:
    ~RecordSlotReader() = default;
};

/** @brief The outcome of one load. */
struct RecordSlotsLoad final {
    std::array<RecordSlotState, 2> states{};
    /** @brief The slot holding the newest valid record, or kNoRecordSlot. */
    std::uint8_t slot{kNoRecordSlot};
    /** @brief Generation of that record; 0 when no slot is valid. */
    std::uint32_t generation{};

    [[nodiscard]] bool loaded() const { return slot != kNoRecordSlot; }
    [[nodiscard]] bool any(const RecordSlotState state) const {
        return states[0] == state || states[1] == state;
    }
    /** @brief Whether both slots were read: each is valid, invalid or absent. */
    [[nodiscard]] bool readable() const {
        return !any(RecordSlotState::unavailable) && !any(RecordSlotState::failed);
    }
    /** @brief Whether nothing but the loaded record (or nothing at all) is stored. */
    [[nodiscard]] bool clean() const {
        for (std::uint8_t index = 0U; index < states.size(); ++index) {
            if (index != slot && states[index] != RecordSlotState::absent &&
                states[index] != RecordSlotState::valid)
                return false;
        }
        return true;
    }
};

/** @brief The two files of one A/B record. */
class RecordSlots final {
  public:
    constexpr RecordSlots(const char* const slot_a, const char* const slot_b)
        : paths_{slot_a, slot_b} {}

    /** @brief The slot a save writes after @p loaded was loaded: the other one, or A. */
    [[nodiscard]] static constexpr std::uint8_t target(const std::uint8_t loaded) {
        return loaded == 0U ? 1U : 0U;
    }

    [[nodiscard]] const char* path(const std::uint8_t slot) const { return paths_[slot]; }

    /**
     * @brief Reads both slots into @p buffer and selects the newest valid record.
     * @param buffer Caller-owned storage for one record; it holds the last slot read on return.
     */
    [[nodiscard]] RecordSlotsLoad load(
        std::uint8_t* buffer, std::size_t capacity, RecordSlotReader& reader) const;

    /**
     * @brief Writes a complete record into target(@p loaded).
     *
     * On failure the loaded slot is untouched and the target holds its old content or the
     * complete record.
     */
    [[nodiscard]] filesystem::MutationResult write(
        std::uint8_t loaded, const void* data, std::size_t size) const;

    /** @brief Removes one slot; a missing slot counts as removed. */
    [[nodiscard]] filesystem::MutationResult remove(std::uint8_t slot) const;

    /**
     * @brief Removes both slots, the one not holding @p newest first, so a failure in between
     * keeps the newest record.
     */
    [[nodiscard]] filesystem::MutationResult remove_both(
        std::uint8_t newest = kNoRecordSlot) const;

  private:
    std::array<const char*, 2> paths_;
};

} // namespace newmota
