/*
  energy_live_daily.cpp - Confirmed signed daily projection independent of trusted civil time

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  EnergyUpdateToday/Energy200ms reference: Tasmota xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later. Independent exact-unit
  implementation; no upstream floating-point settings layout is copied.
*/
/** @file energy_live_daily.cpp
 * @brief Separates RTC-independent confirmed daily accrual from trusted rollover and dated history.
 */
#include "services/energy/energy_service.h"
#include "services/energy/energy_sum.h"
#include "services/time/time_service.h"
#include "newmota/log.h"

namespace newmota::energy {
namespace {
/** @brief Source rollover compares day-of-year rather than a lifetime or elapsed-time total. */
unsigned ordinal(std::uint32_t day) {
    const auto civil = TimeService::civil_time(day * 86400);
    constexpr unsigned starts[]{0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const bool leap = civil.year % 4 == 0 && (civil.year % 100 != 0 || civil.year % 400 == 0);
    return starts[civil.month - 1] + civil.day + (leap && civil.month > 2);
}
} // namespace

void Service::initialize_live_daily(Provider& instance) {
    for (std::size_t c = 0; instance.channels && c < instance.configuration.channels; ++c) {
        auto& channel = instance.channels[c];
        auto& live = channel.live_daily;
        if (!live.present) {
            live.present = true;
            const auto& account = channel.account;
            live.incomplete =
                account.imported.wh || account.imported.micro_wh || account.exported.wh ||
                account.exported.micro_wh || account.import_baseline.wh ||
                account.import_baseline.micro_wh || account.export_baseline.wh ||
                account.export_baseline.micro_wh || account.total_adjustment.magnitude.wh ||
                account.total_adjustment.magnitude.micro_wh ||
                account.export_adjustment.magnitude.wh ||
                account.export_adjustment.magnitude.micro_wh || account.baseline_fields ||
                account.import_resets || account.export_resets;
            /** Migration may recover a recorded daily projection, never a lifetime difference.
             * Old files cannot recover discarded fractional/source carries or unknown-date energy.
             */
            if (channel.calendar && channel.calendar->days[0].present) {
                live.value = channel.calendar->days[0].compatibility;
                live.incomplete = true;
                instance.live_daily_day = channel.calendar->days[0].day;
                instance.live_daily_day_known = true;
            }
            dirty_ = true;
        } else if (!instance.live_daily_collected) {
            live.incomplete = true;
            dirty_ = true;
        }
    }
    instance.live_daily_collected = true;
}

void Service::observe_live_daily(
    Provider& instance, const DirectionalEnergy* deltas, const std::uint8_t* groups) {
    const auto count = instance.configuration.channels;
    if (!instance.channels || !deltas || !groups)
        return;
    bool changed_state = false;
    for (std::size_t c = 0; c < count; ++c)
        if (deltas[c].imported.wh || deltas[c].imported.micro_wh || deltas[c].exported.wh ||
            deltas[c].exported.micro_wh)
            changed_state = true;
    for (std::size_t first = 0; first < count;) {
        auto end = first + 1;
        while (end < count && groups[end] == groups[first])
            ++end;
        std::array<SignedEnergy, kMaximumChannels> previous_carry{};
        for (std::size_t c = 0; c < count; ++c)
            previous_carry[c] = instance.channels[c].live_daily.carry;
        for (auto c = first; c < end; ++c) {
            auto& live = instance.channels[c].live_daily;
            detail::EnergySum sum{};
            sum.add(live.carry);
            sum.add({deltas[c].imported, false});
            sum.add({deltas[c].exported, true});
            if (!live.fault && !sum.value(live.carry))
                live.fault = true;
        }
        /** Each source update examines every carry. Release only when the carry truncated
         * to10microWh source units is strictly greater than1000 in magnitude; preserve
         * exact finer carry and release whole0.01Wh quanta. SO162 discards only a released
         * negative piece, after per-phase netting, never raw export observations.
         */
        for (std::size_t c = 0; c < count; ++c) {
            auto& live = instance.channels[c].live_daily;
            if (live.fault)
                continue;
            const auto magnitude = live.carry.magnitude;
            if (magnitude.wh || magnitude.micro_wh / 10 > 1000) {
                const EnergyValue released{magnitude.wh, magnitude.micro_wh / 10000 * 10000};
                if ((!no_export_today_ || !live.carry.negative) &&
                    !live.value.add(released, live.carry.negative)) {
                    live.fault = true;
                    live.carry = previous_carry[c];
                    AddLog(LogLevel::error, "NRG: live daily phase%u exhausted", unsigned(c + 1));
                    continue;
                }
                live.carry.magnitude = {0, magnitude.micro_wh % 10000};
                if (!live.carry.magnitude.micro_wh)
                    live.carry.negative = false;
            }
        }
        first = end;
    }
    if (now_ >= 11000)
        live_daily_rollover_armed_ = true;
    const auto day = calendar_local_ / 86400;
    const bool midnight = calendar_valid_ && calendar_local_ % 86400 == 0;
    const bool changed = calendar_valid_ && (!instance.live_daily_day_known ||
                                                ordinal(day) != ordinal(instance.live_daily_day));
    const bool new_midnight =
        midnight && (!instance.live_daily_midnight_seen || instance.live_daily_midnight != day);
    /** Confirmed advances arrive before source rollover. Only trusted time can roll;
     * backward day-of-year changes also roll, unlike CalendarData's retained history.
     * Unknown/default day is source day0: do not install a trusted discriminator
     * before uptime>10, or an early synchronization would bypass its first rollover.
     */
    if (live_daily_rollover_armed_ && (changed || new_midnight))
        for (std::size_t c = 0; c < count; ++c) {
            changed_state = true;
            auto& channel = instance.channels[c];
            auto& live = channel.live_daily;
            /** Energy200ms replaces retained Yesterday before resetting live Today. An
             * incomplete/faulted projection cannot manufacture a completed-day value.
             * CalendarData has already rotated separately; it never supplies this reset.
             */
            channel.yesterday_present = live.present && !live.incomplete && !live.fault;
            channel.yesterday = channel.yesterday_present ? live.value : SignedEnergy{};
            /** Remove an explicit seed before retaining measured fractional Wh: a negative
             * seed must not reverse the sign of positive raw fractional consumption.
             * Migration/excluded collection cannot recover an unknown fraction, so a
             * trusted boundary clears the unknown released fraction; the retained
             * confirmed unreleased carry survives independently, as in the source.
             */
            SignedEnergy measured = live.value;
            if (live.incomplete || live.fault ||
                !measured.add(live.offset.magnitude, !live.offset.negative)) {
                measured = {};
            }
            measured.magnitude.wh = 0;
            if (!measured.magnitude.micro_wh)
                measured.negative = false;
            live.value = measured;
            live.offset = {};
            live.incomplete = false;
            live.fault = false;
        }
    if (calendar_valid_ && (!changed || live_daily_rollover_armed_)) {
        changed_state |= !instance.live_daily_day_known || instance.live_daily_day != day;
        instance.live_daily_day = day;
        instance.live_daily_day_known = true;
    }
    if (midnight && live_daily_rollover_armed_) {
        instance.live_daily_midnight = day;
        instance.live_daily_midnight_seen = true;
    }
    /** Existing checkpoint cadence persists value, carry, fault and discriminator together. */
    if (changed_state)
        dirty_ = true;
}
} // namespace newmota::energy
