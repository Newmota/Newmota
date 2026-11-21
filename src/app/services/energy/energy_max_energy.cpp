/*
  energy_max_energy.cpp - Primary daily-energy threshold and start-hour protection

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  EnergyMarginCheck/Energy200ms/CmndMaxEnergy reference: Tasmota xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later.
*/
/** @file energy_max_energy.cpp
 * @brief Uses confirmed live daily values, not dated CalendarData or lifetime differences.
 */
#include "services/energy/energy_service.h"
#include "services/energy/energy_sum.h"
#include "services/power/power_service.h"
#include "services/time/time_service.h"
#include "core/command/command.h"
#include "support/number.h"
#include "support/text.h"
#include "newmota/log.h"
#include <new>

namespace newmota::energy {
void Service::update_max_energy_clock(bool observe) {
    auto& primary = provider_;
    const auto count = primary.configuration.channels;
    if (max_energy_identity_ != primary.generation || max_energy_count_ != count) {
        max_energy_identity_ = primary.generation;
        max_energy_count_ = count;
        max_energy_phases_.reset(count ? new (std::nothrow) MaxEnergyPhase[count] : nullptr);
        if (count && !max_energy_phases_)
            AddLog(LogLevel::error, "NRG: MaxEnergy allocation failed");
    }
    if (!observe || !max_energy_phases_ || !calendar_valid_)
        return;
    const auto day = calendar_local_ / 86400;
    if (calendar_local_ % 86400 == 0 &&
        (!max_energy_midnight_seen_ || max_energy_midnight_ != day)) {
        for (std::size_t c = 0; c < count; ++c)
            max_energy_phases_[c].state = 3;
        max_energy_midnight_ = day;
        max_energy_midnight_seen_ = true;
    }
    const auto hour = TimeService::civil_time(calendar_local_).hour;
    if (hour != max_energy_hour_) {
        max_energy_hour_ = hour;
        for (std::size_t c = 0; c < count; ++c)
            if (max_energy_phases_[c].state == 3 && hour == primary.channels[c].max_energy[1])
                max_energy_phases_[c].state = 0;
    }
}

CommandStatus Service::max_energy_command(const CommandRequest& request, CommandOutput& output) {
    const bool start = support::ascii_iequals(request.name, "MaxEnergyStart");
    if (!start && !support::ascii_iequals(request.name, "MaxEnergy"))
        return CommandStatus::unknown;
    update_max_energy_clock(false);
    auto& primary = provider_;
    const auto count = primary.configuration.channels;
    const std::size_t index = request.has_index ? request.index : 1;
    if (!count || !primary.channels || index > count)
        return CommandStatus::invalid;
    if (!max_energy_phases_)
        return CommandStatus::failed;
    const unsigned field = start ? 1 : 0;
    std::uint32_t value{};
    if (!request.is_query() &&
        support::parse_unsigned_decimal(request.payload, start ? 23 : 6000, value)) {
        std::array<std::uint16_t, kMaximumChannels> previous{};
        for (std::size_t c = 0; c < count; ++c) {
            previous[c] = primary.channels[c].max_energy[field];
            if (!index || index == c + 1)
                primary.channels[c].max_energy[field] = value;
        }
        const auto was_dirty = dirty_;
        dirty_ = true;
        if (!save()) {
            for (std::size_t c = 0; c < count; ++c)
                primary.channels[c].max_energy[field] = previous[c];
            dirty_ = was_dirty;
            return CommandStatus::failed;
        }
        /** Source MaxEnergy0 writes state[index-1] out of bounds after fanout.
         * Reset exactly the selected phases safely; Start setters never reset state.
         */
        if (!start)
            for (std::size_t c = 0; c < count; ++c)
                if (!index || index == c + 1)
                    max_energy_phases_[c].state = 3;
    }
    const char* name = start ? "MaxEnergyStart" : "MaxEnergy";
    if (index)
        return output.format("{\"%s%u\":%u}", name, unsigned(index),
                   unsigned(primary.channels[index - 1].max_energy[field]))
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    if (!output.format("{\"%s\":", name) || (count > 1 && !output.write("[")))
        return CommandStatus::failed;
    for (std::size_t c = 0; c < count; ++c)
        if (!output.format("%s%u", c ? "," : "", unsigned(primary.channels[c].max_energy[field])))
            return CommandStatus::failed;
    return (count > 1 ? output.write("]}") : output.write("}")) ? CommandStatus::handled
                                                                : CommandStatus::failed;
}

void Service::poll_max_energy() {
    auto& primary = provider_;
    const auto count = primary.configuration.channels;
    if (!primary.device || !max_energy_phases_)
        return;
    bool group = true;
    for (std::size_t c = 1; c < count; ++c)
        if (primary.channels[c].max_energy[0])
            group = false;
    /** As in source MarginCheck, the current hour is readable even without trusted RTC.
     * Invalid time does not inhibit complete live energy; only trusted clock transitions rearm.
     */
    const auto hour =
        protection_time_
            ? TimeService::civil_time(protection_time_->snapshot(now_).local_seconds).hour
            : 0;
    for (std::size_t c = 0; c < count; ++c) {
        const auto& setting = primary.channels[c].max_energy;
        if (!setting[0])
            continue;
        auto& phase = max_energy_phases_[c];
        detail::EnergySum sum{};
        bool available = true;
        const auto first = group ? 0 : c, end = group ? count : c + 1;
        for (auto i = first; i < end; ++i) {
            const auto& live = primary.channels[i].live_daily;
            available &= live.present && !live.fault && !live.incomplete;
            sum.add(live.value);
        }
        SignedEnergy daily{};
        std::uint32_t selected{}, on{};
        available =
            available && sum.value(daily) && protection_targets(group ? 0 : c + 1, selected, on);
        if (!available) {
            if (!phase.unavailable)
                AddLog(
                    LogLevel::error, "NRG: MaxEnergy%u data/targets unavailable", unsigned(c + 1));
            phase.unavailable = true;
            continue;
        }
        phase.unavailable = false;
        /** Define source uint16 integral Wh narrowing without overflowing a wide signed cast. */
        auto wh = static_cast<std::uint16_t>(daily.magnitude.wh);
        if (daily.negative)
            wh = static_cast<std::uint16_t>(0U - wh);
        if (!phase.state && hour == setting[1]) {
            phase.state = 1;
            publish_protection_event(ProtectionEvent::energy_start, c + 1);
            const auto result =
                protection_retry(group ? 0 : c + 1, PowerOperationSource::max_energy);
            if (result != PowerChangeResult::changed && result != PowerChangeResult::unchanged)
                AddLog(LogLevel::error, "NRG: MaxEnergy%u start failed", unsigned(c + 1));
        } else if (phase.state == 1 && wh >= setting[0]) {
            phase.state = 2;
            publish_protection_event(ProtectionEvent::energy_trip, c + 1, 0, &daily);
            const auto result =
                protection_trip(group ? 0 : c + 1, PowerOperationSource::max_energy);
            if (result != PowerChangeResult::changed && result != PowerChangeResult::unchanged)
                AddLog(LogLevel::error, "NRG: MaxEnergy%u cutoff failed", unsigned(c + 1));
        }
    }
}
} // namespace newmota::energy
