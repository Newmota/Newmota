/*
  global_temperature_selection.cpp - Typed global-temperature ordinal and presentation

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  GetSensorValues formatted-temperature selection reference: Tasmota support_tasmota.ino,
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
*/

/** @file global_temperature_selection.cpp
 * @brief Preserves manual precedence, field fallback and presentation round trip without JSON
 * scans.
 */
#include "services/temperature/global_temperature_selection.h"
#include "services/temperature/global_temperature.h"
#include "services/settings/settings.h"
#include "drivers/sensors/sensor_service.h"
#if USE_ENERGY
#include "services/energy/energy_service.h"
#endif
#include <algorithm>
#include <climits>

namespace newmota {
namespace {
/** @brief Matches the canonical sensor formatter's signed nearest rounding. */
std::int64_t rounded(std::int64_t value, std::int64_t divisor) {
    return value < 0 ? -((-value + divisor / 2) / divisor) : (value + divisor / 2) / divisor;
}

/** @brief Constant-space ordinal collector, retaining first fallback and one selected value.
 * Quantity metadata replaces source substring matching. Energy's typed milli-Celsius
 * fields map to virtual Temperature fields with the same global presentation policy;
 * reported exact physical fields themselves are not rounded or changed.
 */
struct Selection {
    const SensorPresentation& presentation;
    std::uint8_t target;
    std::uint32_t count{};
    std::int32_t first{}, selected{};

    /** @brief Offers one canonical field after bounded SO8/offset/resolution round trip. */
    void offer(std::int64_t celsius) {
        if (celsius < INT32_MIN || celsius > INT32_MAX)
            return;
        auto reported = presentation.fahrenheit ? rounded(celsius * 9, 5) + 32000 : celsius;
        reported += std::int64_t(presentation.temperature_offset) * 100;
        constexpr std::int64_t quantum[]{1000, 100, 10, 1};
        const auto step = quantum[std::min(presentation.temperature_resolution, std::uint8_t{3})];
        reported = rounded(reported, step) * step;
        const auto canonical =
            presentation.fahrenheit ? rounded((reported - 32000) * 5, 9) : reported;
        if (canonical < INT32_MIN || canonical > INT32_MAX)
            return;
        ++count;
        if (count == 1)
            first = static_cast<std::int32_t>(canonical);
        if (count == target)
            selected = static_cast<std::int32_t>(canonical);
    }
};
} // namespace

void refresh_global_temperature_selection(
    const Settings& settings, std::uint32_t now, bool publication) {
    auto& owner = global_temperature();
    if (!owner.selection_due(now, publication))
        return;
    SensorPresentation presentation{};
#if USE_SENSOR_SETTINGS
    presentation = settings.sensor_presentation();
#else
    std::uint8_t fahrenheit{};
    (void)settings.option(8, fahrenheit);
    presentation.fahrenheit = fahrenheit != 0;
#endif
    Selection selection{presentation, owner.source()};
#if USE_SENSORS
    if (!sensor_service().visit_readings(
            [](void* context, const SensorReading& reading) {
                // A dashboard-only reading is a display copy; its owner offers it below.
                if (reading.quantity == SensorQuantity::temperature && !reading.dashboard_only)
                    static_cast<Selection*>(context)->offer(reading.value);
                return true;
            },
            &selection, true))
        return;
#endif
#if USE_ENERGY
    if (!energy::service().visit_temperatures(
            [](void* context, std::int64_t celsius) {
                static_cast<Selection*>(context)->offer(celsius);
                return true;
            },
            &selection))
        return;
#endif
    if (selection.count)
        owner.selected(
            selection.count >= selection.target ? selection.selected : selection.first, now);
}
} // namespace newmota
