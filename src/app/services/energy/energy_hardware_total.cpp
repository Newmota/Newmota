/*
  energy_hardware_total.cpp - Primary hardware-total reference projection

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  EnergyUpdateTotal reference: Tasmota xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later.
*/

/** @file energy_hardware_total.cpp
 * @brief Reconciles only the primary reported Total to accepted source hardware references.
 */
#include "services/energy/energy_service.h"
#include "newmota/log.h"

namespace newmota::energy {
void Service::reconcile_hardware_total(
    const Provider& instance, Channel& channel, const Sample& sample, bool accepted) {
    if (&instance != &provider_ || !instance.device->source_total_counter() || !accepted ||
        !sample.valid || !channel.sample.valid || !hardware_total_enabled_ ||
        !hardware_total_enabled_(hardware_total_context_))
        return;
    if (!sample.import_reference_valid)
        return;
    const auto reference = sample.import_reference;
    if (reference.micro_wh >= 1000000 || channel.account.total_fault)
        return;
    SignedEnergy projected{};
    if (!channel.account.compatibility_total(projected))
        return;
    SignedEnergy difference{reference, false};
    /** @brief Exact normalized arithmetic avoids source float rounding at the strict10Wh boundary.
     * A projection correction is not observed consumption, including when it lowers Total.
     */
    const bool bounded_distance = difference.add(projected.magnitude, !projected.negative);
    /** @brief Opposite-sign extrema may exceed the delta representation while the new adjustment
     * remains representable. Such a distance is certainly greater than10Wh.
     */
    if (bounded_distance && (difference.magnitude.wh < 10 ||
                                (difference.magnitude.wh == 10 && !difference.magnitude.micro_wh)))
        return;
    SignedEnergy adjustment{reference, false};
    if (!adjustment.add(channel.account.imported, true) ||
        !adjustment.add(channel.account.exported)) {
        channel.account.total_fault = true;
        dirty_ = true;
        AddLog(LogLevel::error, "NRG: hardware Total projection exhausted");
        return;
    }
    channel.account.total_adjustment = adjustment;
    /** @brief Timestamp is meter-wide metadata. Unsynchronized fallback time cannot date it. */
    auto& owner = provider_;
    const auto& offset = channel.live_daily.offset.magnitude;
    owner.total_time = !calendar_valid_               ? 0
                       : offset.wh || offset.micro_wh ? calendar_local_ / 86400 * 86400
                                                      : calendar_local_;
    dirty_ = true;
}
} // namespace newmota::energy
