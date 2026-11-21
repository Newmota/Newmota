/*
  energy_calendar.cpp - Exact dated directional energy buckets

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Daily-accounting reference: Tasmota xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later. Independent exact
  directional model; no Tasmota floating-point settings layout is copied.
*/

/** @file energy_calendar.cpp
 * @brief Exact directional daily accounting with explicit uncertain-time retention.
 */
#include "services/energy/energy_service.h"

namespace newmota::energy {
/** @brief Commits both directions together so exhaustion cannot partially update a day. */
static bool add(DirectionalEnergy& target, const DirectionalEnergy& delta) {
    auto next = target;
    if (!next.imported.add(delta.imported) || !next.exported.add(delta.exported))
        return false;
    target = next;
    return true;
}

bool CalendarData::observe(std::uint32_t local, bool valid, bool no_export) {
    auto next = *this;
    DirectionalEnergy* target = &next.unassigned;
    CalendarDay* selected{};
    if (valid) {
        const auto day = local / 86400;
        if (!next.days[0].present) {
            next.days[0] = {day, local, {}, true};
        } else if (day > next.days[0].day) {
            next.days[1] = day - next.days[0].day == 1 ? next.days[0] : CalendarDay{};
            next.days[0] = {day, local, {}, true};
        }
        // A backward correction does not rotate twice or erase the newer observed day.
        for (auto& bucket : next.days)
            if (bucket.present && bucket.day == day) {
                target = &bucket.energy;
                selected = &bucket;
                break;
            }
    }
    if (!add(*target, incoming))
        return false;
    SignedEnergy change{};
    if (selected &&
        (!change.add(incoming.imported) || (!no_export && !change.add(incoming.exported, true)) ||
            !selected->compatibility.add(change.magnitude, change.negative)))
        return false;
    next.incoming = {};
    *this = next;
    return true;
}
} // namespace newmota::energy
