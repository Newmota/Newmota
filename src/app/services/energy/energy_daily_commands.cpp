/*
  energy_daily_commands.cpp - Live Today and retained Yesterday compatibility commands

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Command/accounting reference: Tasmota xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later. Independent
  exact signed model; no upstream floating-point settings layout is copied.
*/

/** @file energy_daily_commands.cpp
 * @brief Live Today and independently retained Yesterday commands with transactional projection
 * seeds.
 */
#include "services/energy/energy_service.h"
#include "core/command/command.h"
#include "services/energy/energy_sum.h"
#include "newmota/platform/platform.h"
#include "support/number.h"
#include "support/text.h"
#include <new>

namespace newmota::energy {
bool Service::seed_daily(std::size_t channel, bool yesterday, std::string_view payload) {
    if (storage_fault_ || !channel || channel > provider_.configuration.channels)
        return false;
    const auto comma = payload.find(',');
    std::int32_t wh{};
    std::uint32_t timestamp{};
    if (!support::parse_signed_decimal(
            support::trim_ascii(payload.substr(0, comma)), INT32_MIN, INT32_MAX, wh) ||
        (comma != std::string_view::npos &&
            !support::parse_unsigned_decimal(
                support::trim_ascii(payload.substr(comma + 1)), UINT32_MAX, timestamp)))
        return false;
    poll(platform::monotonic_milliseconds());
    auto& instance = provider_;
    auto& target = instance.channels[channel - 1];
    if (!yesterday && !target.live_daily.present)
        return false;
    const auto day = calendar_local_ / 86400;
    const auto wanted = day ? day - (yesterday ? 1 : 0) : 0;
    const auto previous = target.calendar ? *target.calendar : CalendarData{};
    CalendarDay* selected{};
    /** Undated seeds never create a CalendarDay. A trusted existing observation may
     * receive a compatibility seed; Yesterday may also create its preceding bucket
     * under the established dated-history policy, separately from the retained value.
     */
    if (calendar_valid_ && (!yesterday || day) && target.calendar && !target.calendar_fault)
        for (auto& bucket : target.calendar->days)
            if (bucket.present && bucket.day == wanted)
                selected = &bucket;
    if (!selected && yesterday && calendar_valid_ && day && target.calendar &&
        !target.calendar_fault && target.calendar->days[0].present &&
        target.calendar->days[0].day == day && !target.calendar->days[1].present) {
        selected = &target.calendar->days[1];
        *selected = {wanted, wanted * 86400, {}, true};
    }
    const auto old_adjustment = target.account.total_adjustment;
    const auto old_time = instance.total_time;
    const auto was_dirty = dirty_;
    const auto previous_live = target.live_daily;
    const auto previous_yesterday = target.yesterday;
    const auto previous_yesterday_present = target.yesterday_present;
    const auto magnitude = wh < 0 ? std::uint64_t(-std::int64_t(wh)) : std::uint64_t(wh);
    const SignedEnergy seed{EnergyValue{magnitude, 0}, wh < 0};
    SignedEnergy change = seed;
    /** Total includes the live Today projection in the source. Shift only its explicit
     * compatibility offset by new-live minus old-live, never an observed direction,
     * hardware counter baseline, tariff delta or invented dated bucket.
     */
    if (!yesterday && (!change.add(previous_live.value.magnitude, !previous_live.value.negative) ||
                          !target.account.total_adjustment.add(change.magnitude, change.negative)))
        return false;
    if (comma != std::string_view::npos)
        instance.total_time = timestamp;
    else if (!yesterday && !magnitude) {
        SignedEnergy base{};
        if (target.account.compatibility_total(base) && !base.magnitude.wh &&
            !base.magnitude.micro_wh)
            instance.total_time = calendar_local_;
    }
    if (yesterday) {
        target.yesterday = seed;
        target.yesterday_present = true;
    } else {
        target.live_daily = {};
        target.live_daily.value = seed;
        target.live_daily.offset = seed;
        target.live_daily.present = true;
    }
    if (selected) {
        selected->compatibility = seed;
        selected->offset_nonzero = magnitude != 0;
        selected->seeded = true;
        selected->seed_time = timestamp;
    }
    dirty_ = true;
    if (save()) {
#if USE_ENERGY_PERIOD_COMPAT
        if (!yesterday)
            reset_period(channel - 1);
#endif
#if USE_ENERGY_BALANCED_DAILY
        if (!yesterday)
            reset_balanced_carry(channel - 1);
#endif
        return true;
    }
    target.live_daily = previous_live;
    target.yesterday = previous_yesterday;
    target.yesterday_present = previous_yesterday_present;
    if (target.calendar)
        *target.calendar = previous;
    target.account.total_adjustment = old_adjustment;
    instance.total_time = old_time;
    dirty_ = was_dirty;
    return false;
}

namespace {
/** @brief Writes exact live/projected kWh; as in Tasmota, an unknown value reads as zero. */
bool write_value(CommandOutput& output, const SignedEnergy* value, unsigned places) {
    detail::EnergySum sum{};
    if (value)
        sum.add(*value);
    return sum.write(output, places);
}

} // namespace

bool Service::write_daily(CommandOutput& output, std::string_view command) const {
    const auto& instance = provider_;
    if (!instance.configuration.channels || !output.write("{") || !output.json_string(command) ||
        !output.write(":{"))
        return false;
#if USE_ENERGY_TOTAL_COMPAT
    if (!output.write("\"Total\":") || (instance.configuration.channels > 1 && !output.write("[")))
        return false;
    for (std::size_t c = 0; c < instance.configuration.channels; ++c) {
        SignedEnergy projection{};
        const bool valid = instance.channels[c].account.compatibility_total(projection);
        if ((c && !output.write(",")) || !write_value(output, valid ? &projection : nullptr,
                                             output_resolution(OutputQuantity::energy)))
            return false;
    }
    if ((instance.configuration.channels > 1 && !output.write("]")) ||
        !output.format(",\"TotalStartTime\":%lu,", static_cast<unsigned long>(instance.total_time)))
        return false;
#endif
    for (unsigned previous = 0; previous < 2; ++previous) {
        if ((previous && !output.write(",")) ||
            !output.format("\"%s\":", previous ? "Yesterday" : "Today") ||
            (instance.configuration.channels > 1 && !output.write("[")))
            return false;
        for (std::size_t c = 0; c < instance.configuration.channels; ++c) {
            const auto& channel = instance.channels[c];
            if (!previous) {
                const auto& live = channel.live_daily;
                if ((c && !output.write(",")) ||
                    !write_value(output,
                        live.present && !live.fault && !live.incomplete ? &live.value : nullptr,
                        output_resolution(OutputQuantity::energy)))
                    return false;
                continue;
            }
            if ((c && !output.write(",")) ||
                !write_value(output, channel.yesterday_present ? &channel.yesterday : nullptr,
                    output_resolution(OutputQuantity::energy)))
                return false;
        }
        if (instance.configuration.channels > 1 && !output.write("]"))
            return false;
    }
#if USE_ENERGY_EXPORT_COMPAT
    if (export_reporting()) {
        if (!output.write(",\"ExportActive\":") ||
            (instance.configuration.channels > 1 && !output.write("[")))
            return false;
        for (std::size_t c = 0; c < instance.configuration.channels; ++c) {
            SignedEnergy projection{};
            const bool valid = instance.channels[c].account.compatibility_export(projection);
            if ((c && !output.write(",")) || !write_value(output, valid ? &projection : nullptr,
                                                 output_resolution(OutputQuantity::energy)))
                return false;
        }
        if (instance.configuration.channels > 1 && !output.write("]"))
            return false;
    }
#endif
    return output.format(",\"ClockValid\":%s,\"NoExport\":%s}}", calendar_valid_ ? "true" : "false",
        no_export_today_ ? "true" : "false");
}

CommandStatus dispatch_daily(const CommandRequest& request, CommandOutput& output) {
    const bool today = support::ascii_iequals(request.name, "EnergyToday");
    const bool yesterday = support::ascii_iequals(request.name, "EnergyYesterday");
    if (!today && !yesterday)
        return CommandStatus::unknown;
    auto& owner = service();
    if (request.is_query())
        owner.poll(platform::monotonic_milliseconds());
    else if (!owner.seed_daily(request.has_index ? request.index : 1, yesterday, request.payload))
        return CommandStatus::failed;
    return owner.write_daily(output, today ? "EnergyToday" : "EnergyYesterday")
               ? CommandStatus::handled
               : CommandStatus::failed;
}
} // namespace newmota::energy
