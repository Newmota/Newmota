/*
  energy_margins.cpp - Primary per-phase measurement threshold transitions

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Margin/command reference: Tasmota xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later.
*/

/** @file energy_margins.cpp
 * @brief Read-only measurement comparisons and retained primary reporting policy, not protection.
 */
#include "services/energy/energy_service.h"
#include "core/command/command.h"
#include "support/text.h"
#include "services/settings/state_text.h"
#include "support/number.h"

namespace newmota::energy {
namespace {
constexpr const char* names[]{
    "PowerLow", "PowerHigh", "VoltageLow", "VoltageHigh", "CurrentLow", "CurrentHigh"};
}

CommandStatus Service::margin_command(const CommandRequest& request, CommandOutput& output) {
    for (unsigned field = 0; field < 6; ++field) {
        if (!support::ascii_iequals(request.name, names[field]))
            continue;
        auto& primary = provider_;
        const auto count = primary.configuration.channels;
        const std::size_t index = request.has_index ? request.index : 0;
        if (!count || !primary.channels || index > count)
            return CommandStatus::invalid;
        std::uint32_t value{};
        const auto maximum = field < 2 ? 6000U : field < 4 ? 500U : 25000U;
        if (!request.is_query() &&
            support::parse_unsigned_decimal(request.payload, maximum, value)) {
            std::array<std::uint16_t, kMaximumChannels> previous{};
            for (std::size_t c = 0; c < count; ++c) {
                previous[c] = primary.channels[c].margins[field];
                if (!index || c + 1 == index)
                    primary.channels[c].margins[field] = value;
            }
            const auto was_dirty = dirty_;
            dirty_ = true;
            if (!save()) {
                for (std::size_t c = 0; c < count; ++c)
                    primary.channels[c].margins[field] = previous[c];
                dirty_ = was_dirty;
                return CommandStatus::failed;
            }
        }
        if (index)
            return output.format("{\"%s%u\":%u}", names[field], unsigned(index),
                       unsigned(primary.channels[index - 1].margins[field]))
                       ? CommandStatus::handled
                       : CommandStatus::failed;
        if (count == 1)
            return output.format(
                       "{\"%s\":%u}", names[field], unsigned(primary.channels[0].margins[field]))
                       ? CommandStatus::handled
                       : CommandStatus::failed;
        if (!output.format("{\"%s\":[", names[field]))
            return CommandStatus::failed;
        for (std::size_t c = 0; c < count; ++c)
            if (!output.format("%s%u", c ? "," : "", unsigned(primary.channels[c].margins[field])))
                return CommandStatus::failed;
        return output.write("]}") ? CommandStatus::handled : CommandStatus::failed;
    }
    return CommandStatus::unknown;
}

std::uint8_t Service::margin_changes(std::size_t channel) {
    const auto& phase = provider_.channels[channel];
    const auto& sample = phase.sample;
    if (!sample.presentation_valid())
        return 0;
    auto& flags = delta_phases_[channel].margin_flags;
    std::uint8_t changes{};
    for (unsigned field = 0; field < 6; ++field) {
        const auto setting = phase.margins[field];
        const auto mask = field < 2 ? 4 : field < 4 ? 1 : 2;
        if (!setting || !(sample.available_fields & mask))
            continue;
        /** Preserve source integral W/V/mA and define unsigned narrowing for negative readings. */
        const auto value = std::uint16_t(field < 2   ? sample.power / 1000
                                         : field < 4 ? sample.voltage / 1000
                                                     : sample.current);
        const auto bit = std::uint8_t(1U << field);
        const bool active = field & 1 ? value > setting : value < setting;
        if (active != bool(flags & bit))
            changes |= bit;
        flags = active ? flags | bit : flags & ~bit;
    }
    return changes;
}

bool Service::write_margin_events(
    CommandOutput& output, const std::uint8_t* changes, bool& first) const {
    for (std::size_t c = 0; c < delta_count_; ++c)
        for (unsigned field = 0; field < 6; ++field) {
            const auto bit = std::uint8_t(1U << field);
            if (!(changes[c] & bit))
                continue;
            if (!output.format("%s\"%s%u\":", first ? "" : ",", names[field], unsigned(c + 1)) ||
                !output.json_string(state_text(bool(delta_phases_[c].margin_flags & bit))))
                return false;
            first = false;
        }
    return true;
}
} // namespace newmota::energy
