/*
  energy_max_power.cpp - Explicitly targeted primary maximum-power protection

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  EnergyMarginCheck/CmndMaxPower reference: Tasmota xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later.
*/
/** @file energy_max_power.cpp
 * @brief Source hold/retry/window ordering over explicit Newmota output ownership.
 */
#include "services/energy/energy_service.h"
#include "services/power/power_service.h"
#include "services/settings/settings.h"
#include "services/settings/state_text.h"
#include "services/time/time_service.h"
#include "core/command/command.h"
#include "support/number.h"
#include "support/text.h"
#include "newmota/log.h"
#include <algorithm>
#include <new>

namespace newmota::energy {
CommandStatus Service::max_power_command(const CommandRequest& request, CommandOutput& output) {
    constexpr const char* names[]{"MaxPower", "MaxPowerHold", "MaxPowerWindow"};
    unsigned field = 0;
    while (field < 3 && !support::ascii_iequals(request.name, names[field]))
        ++field;
    if (field == 3)
        return CommandStatus::unknown;
    auto& primary = provider_;
    const auto count = primary.configuration.channels;
    const std::size_t index = request.has_index ? request.index : 1;
    if (!primary.channels || !count || index > count)
        return CommandStatus::invalid;
    std::uint32_t value{};
    if (!request.is_query() && support::parse_unsigned_decimal(request.payload, 6000, value)) {
        constexpr std::uint16_t defaults[]{1, 10, 30};
        if (value == 1)
            value = defaults[field];
        std::array<std::uint16_t, kMaximumChannels> previous{};
        for (std::size_t c = 0; c < count; ++c) {
            previous[c] = primary.channels[c].max_power[field];
            if (!index || index == c + 1)
                primary.channels[c].max_power[field] = value;
        }
        const auto was_dirty = dirty_;
        dirty_ = true;
        if (!save()) {
            for (std::size_t c = 0; c < count; ++c)
                primary.channels[c].max_power[field] = previous[c];
            dirty_ = was_dirty;
            return CommandStatus::failed;
        }
        /** Source setters do not clear pending hold/retry/window counters. */
    }
    if (index)
        return output.format("{\"%s%u\":%u}", names[field], unsigned(index),
                   unsigned(primary.channels[index - 1].max_power[field]))
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    if (!output.format("{\"%s\":", names[field]) || (count > 1 && !output.write("[")))
        return CommandStatus::failed;
    for (std::size_t c = 0; c < count; ++c)
        if (!output.format("%s%u", c ? "," : "", unsigned(primary.channels[c].max_power[field])))
            return CommandStatus::failed;
    return ((count > 1 ? output.write("]}") : output.write("}"))) ? CommandStatus::handled
                                                                  : CommandStatus::failed;
}

void Service::poll_max_power(std::uint32_t now) {
    auto& primary = provider_;
    const auto count = primary.configuration.channels;
    if (max_power_identity_ != primary.generation || max_power_count_ != count) {
        max_power_identity_ = primary.generation;
        max_power_count_ = count;
        max_power_phases_.reset(count ? new (std::nothrow) MaxPowerPhase[count] : nullptr);
        if (count && !max_power_phases_)
            AddLog(LogLevel::error, "NRG: MaxPower allocation failed");
    }
    (void)now;
    if (!primary.device || !max_power_phases_)
        return;
    bool group = true;
    for (std::size_t c = 1; c < count; ++c)
        if (primary.channels[c].max_power[0])
            group = false;
    for (std::size_t c = 0; c < count; ++c) {
        auto& state = max_power_phases_[c];
        const auto& phase = primary.channels[c];
        const auto& settings = phase.max_power;
        const auto& sample = phase.sample;
        if (!settings[0] || !sample.presentation_valid() || !(sample.available_fields & 4))
            continue;
        std::uint32_t selection{}, on{}, group_selection{}, group_on{};
        const bool ready = protection_targets(c + 1, selection, on) &&
                           (!group || protection_targets(0, group_selection, group_on));
        if (!ready) {
            if (!state.unavailable)
                AddLog(LogLevel::error, "NRG: MaxPower%u targets unavailable", unsigned(c + 1));
            state.unavailable = true;
            continue;
        }
        state.unavailable = false;
        /** Explicit narrowing matches uint16 source W, including negative-export wrap.
         * Capture channel intent before any trip; the bottom branch uses this old value.
         */
        const auto watts = static_cast<std::uint16_t>(sample.power / 1000);
        const bool power_on = on != 0;
        if (watts > settings[0]) {
            if (!state.hold)
                state.hold = settings[1] + 1;
            if (!--state.hold) {
                if (!state.retry) {
                    std::uint8_t retries{};
                    (void)protection_settings_->option(33, retries);
                    state.retry = static_cast<std::uint8_t>(retries + 1);
                }
                --state.retry;
                publish_protection_event(
                    state.retry ? ProtectionEvent::power_trip : ProtectionEvent::power_exhausted,
                    c + 1, watts);
                const auto result =
                    protection_trip(group ? 0 : c + 1, PowerOperationSource::max_power);
                if (result != PowerChangeResult::changed && result != PowerChangeResult::unchanged)
                    AddLog(LogLevel::error, "NRG: MaxPower%u cutoff failed", unsigned(c + 1));
                state.window = settings[2];
            }
        } else if (power_on)
            state.hold = state.retry = state.window = 0;
        if (!power_on) {
            if (state.window)
                --state.window;
            else if (state.retry) {
                publish_protection_event(ProtectionEvent::power_retry, c + 1, watts);
                const auto result =
                    protection_retry(group ? 0 : c + 1, PowerOperationSource::max_power);
                if (result != PowerChangeResult::changed && result != PowerChangeResult::unchanged)
                    AddLog(LogLevel::error, "NRG: MaxPower%u restore failed", unsigned(c + 1));
            }
        }
    }
}
} // namespace newmota::energy
