/*
  energy_balanced_daily.cpp - Runtime balanced daily energy from grouped phase advances

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Balanced-day reference: Tasmota xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later. Independent
  exact confirmed-delta implementation; no upstream settings layout is copied.
*/

/** @file energy_balanced_daily.cpp
 * @brief Balances one complete meter poll vector before splitting directional daily totals.
 */
#include "services/energy/energy_service.h"
#include "services/energy/energy_sum.h"
#include "services/time/time_service.h"
#include <new>

namespace newmota::energy {
namespace {
/** @brief Derives the source day-of-year discriminator from a retained full local date. */
unsigned day_of_year(std::uint32_t day) {
    const auto civil = TimeService::civil_time(day * 86400);
    constexpr unsigned starts[]{0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const bool leap = civil.year % 4 == 0 && (civil.year % 100 != 0 || civil.year % 400 == 0);
    return starts[civil.month - 1] + civil.day + (leap && civil.month > 2 ? 1 : 0);
}

} // namespace

BalancedDaily::BalancedDaily(std::size_t channels)
    : carries(channels && channels <= Service::kMaximumChannels ? new (std::nothrow)
                                                                      SignedEnergy[channels]
                                                                : nullptr),
      count(channels) {}

bool BalancedDaily::observe(const DirectionalEnergy* deltas, const std::uint8_t* groups,
    std::size_t channels, std::uint32_t local, bool valid, bool allow_rollover) {
    if (!carries || !deltas || !groups || channels != count || channels > Service::kMaximumChannels)
        return false;
    std::array<SignedEnergy, Service::kMaximumChannels> next{};
    for (std::size_t c = 0; c < count; ++c)
        next[c] = carries[c];
    DirectionalEnergy advances{};
    for (std::size_t first = 0; first < count;) {
        auto end = first + 1;
        while (end < count && groups[end] == groups[first])
            ++end;
        for (auto c = first; c < end; ++c) {
            detail::EnergySum carry{};
            carry.add(next[c]);
            carry.add(SignedEnergy{deltas[c].imported, false});
            carry.add(SignedEnergy{deltas[c].exported, true});
            if (!carry.value(next[c]))
                return false;
        }
        detail::EnergySum released{};
        // Every source update examines all phase carries, including phases not changed this run.
        for (std::size_t c = 0; c < count; ++c) {
            auto& value = next[c];
            // Source abs(delta)>1000 is strict after truncating carry to 10microWh units.
            // Retain additional exact sub-unit carry; release only whole 0.01Wh quanta.
            if (value.magnitude.wh || value.magnitude.micro_wh / 10 > 1000) {
                const auto fraction = value.magnitude.micro_wh / 10000 * 10000;
                released.add(
                    SignedEnergy{EnergyValue{value.magnitude.wh, fraction}, value.negative});
                value.magnitude = {0, value.magnitude.micro_wh % 10000};
                if (!value.magnitude.micro_wh)
                    value.negative = false;
            }
        }
        SignedEnergy net{};
        if (!released.value(net) ||
            !(net.negative ? advances.exported : advances.imported).add(net.magnitude))
            return false;
        first = end;
    }
    auto runtime_next = runtime;
    const auto day = local / 86400;
    const bool midnight = valid && local % 86400 == 0;
    const bool changed_day = valid && day_known && day_of_year(day) != day_of_year(observed_day);
    const bool new_midnight = midnight && (!midnight_seen || midnight_day != day);
    if (day_known && allow_rollover && (changed_day || new_midnight))
        runtime_next = {}; // Source volatile reset includes backward dates; carries survive.
    if (!runtime_next.imported.add(advances.imported) ||
        !runtime_next.exported.add(advances.exported))
        return false;
    auto dated = calendar;
    dated.incoming = advances;
    // Shared date policy retains the previous observed day and never assigns unknown dates.
    if (!dated.observe(local, valid))
        return false;
    calendar = dated;
    runtime = runtime_next;
    if (valid && (!day_known || !changed_day || allow_rollover)) {
        observed_day = day;
        day_known = true;
    }
    if (midnight && allow_rollover) {
        midnight_day = day;
        midnight_seen = true;
    }
    for (std::size_t c = 0; c < count; ++c)
        carries[c] = next[c];
    return true;
}

void Service::reset_balanced_carry(std::size_t channel) {
    auto& instance = provider_;
    if (instance.balanced && instance.balanced->carries && channel < instance.balanced->count)
        instance.balanced->carries[channel] = {};
}

} // namespace newmota::energy
