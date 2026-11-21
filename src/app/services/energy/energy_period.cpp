/*
  energy_period.cpp - Accepted-telemetry energy intervals

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Period/reset reference: Tasmota xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later. Independent
  confirmed-delta implementation; no upstream floating settings layout is copied.
*/

/** @file energy_period.cpp
 * @brief Runtime interval previews committed only after successful SENSOR transport acceptance.
 */
#include "services/energy/energy_service.h"
#include "core/command/command.h"
#include "services/energy/energy_sum.h"
#include <new>

namespace newmota::energy {
void Service::synchronize_period() {
    const auto& primary = provider_;
    const auto count = primary.configuration.channels;
    if (period_identity_generation_ == primary.generation && period_count_ == count)
        return;
    period_token_ = 0;
    period_identity_generation_ = primary.generation;
    period_count_ = count;
    periods_.reset(count ? new (std::nothrow) PeriodChannel[count] : nullptr);
    period_allocation_fault_ = count && !periods_;
}

void Service::observe_period(std::size_t channel, const DirectionalEnergy* delta) {
    if (!periods_ || channel >= period_count_)
        return;
    auto& period = periods_[channel];
    if (period.fault)
        return;
    if (!delta) {
        period.fault = true;
        return;
    }
    detail::EnergySum sum{};
    sum.add(period.pending);
    sum.add(SignedEnergy{delta->imported, false});
    if (!no_export_today_)
        sum.add(SignedEnergy{delta->exported, true});
    SignedEnergy next{};
    if (!sum.value(next)) {
        period.fault = true;
        return;
    }
    period.pending = next;
}

bool Service::prepare_period(std::uint32_t& token) {
    synchronize_period();
    token = 0;
    if (!period_count_)
        return true;
    if (!++period_generation_)
        ++period_generation_;
    token = period_token_ = period_generation_;
    if (periods_)
        for (std::size_t c = 0; c < period_count_; ++c)
            periods_[c].prepared = periods_[c].pending;
    return true;
}

void Service::accept_period(std::uint32_t token) {
    // A reset/replacement or newer preparation invalidates the previous document's token.
    if (!token || token != period_token_ || period_identity_generation_ != provider_.generation)
        return;
    if (periods_)
        for (std::size_t c = 0; c < period_count_; ++c) {
            auto& period = periods_[c];
            if (!period.fault &&
                !period.pending.add(period.prepared.magnitude, !period.prepared.negative))
                period.fault = true;
        }
    period_token_ = 0;
}

void Service::reset_period(std::size_t channel) {
    synchronize_period();
    if (periods_ && channel < period_count_) {
        periods_[channel] = {};
        period_token_ = 0;
    }
}

bool Service::write_period(CommandOutput& output) const {
    const auto count = provider_.configuration.channels;
    if (count > 1 && !output.write("["))
        return false;
    for (std::size_t c = 0; c < count; ++c) {
        if (c && !output.write(","))
            return false;
        // As in Tasmota, an unknown period reads as zero.
        const bool known = period_token_ && !period_allocation_fault_ && periods_ &&
                           c < period_count_ && !periods_[c].fault;
        const auto value = known ? periods_[c].prepared : SignedEnergy{};
        if (!detail::write_decimal(output, value.negative, value.magnitude.wh,
                value.magnitude.micro_wh, 6, output_resolution(OutputQuantity::power)))
            return false;
    }
    return count <= 1 || output.write("]");
}
} // namespace newmota::energy
