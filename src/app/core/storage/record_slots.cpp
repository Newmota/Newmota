/*
  record_slots.cpp - Two-slot (A/B) record files for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file record_slots.cpp
 * @brief Slot selection by serial generation order and single-write replacement.
 */
#include "core/storage/record_slots.h"

namespace newmota {

RecordSlotsLoad RecordSlots::load(
    std::uint8_t* const buffer, const std::size_t capacity, RecordSlotReader& reader) const {
    RecordSlotsLoad result{};
    for (std::uint8_t slot = 0U; slot < paths_.size(); ++slot) {
        std::size_t size = 0U;
        auto& state = result.states[slot];
        switch (filesystem::read_file(paths_[slot], buffer, capacity, size)) {
        case filesystem::ReadResult::succeeded:
            break;
        case filesystem::ReadResult::not_found:
            state = RecordSlotState::absent;
            continue;
        case filesystem::ReadResult::too_large:
            state = RecordSlotState::invalid;
            continue;
        case filesystem::ReadResult::unavailable:
            state = RecordSlotState::unavailable;
            continue;
        case filesystem::ReadResult::invalid_path:
        case filesystem::ReadResult::failed:
            state = RecordSlotState::failed;
            continue;
        }
        if (size == 0U) {
            state = RecordSlotState::absent;
            continue;
        }
        std::uint32_t generation = 0U;
        if (!reader.check(slot, buffer, size, generation)) {
            state = RecordSlotState::invalid;
            continue;
        }
        state = RecordSlotState::valid;
        if (!result.loaded() || newer_generation(generation, result.generation)) {
            result.slot = slot;
            result.generation = generation;
            reader.keep(slot, buffer, size);
        }
    }
    return result;
}

filesystem::MutationResult RecordSlots::write(
    const std::uint8_t loaded, const void* const data, const std::size_t size) const {
    return filesystem::write_file(paths_[target(loaded)], data, size);
}

filesystem::MutationResult RecordSlots::remove(const std::uint8_t slot) const {
    return filesystem::remove_if_exists(paths_[slot]);
}

filesystem::MutationResult RecordSlots::remove_both(const std::uint8_t newest) const {
    const auto first = target(newest);
    const auto result = remove(first);
    return result == filesystem::MutationResult::succeeded ? remove(target(first)) : result;
}

} // namespace newmota
