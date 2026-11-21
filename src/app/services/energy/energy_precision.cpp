/*
  energy_precision.cpp - Retained source energy formatting commands

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Command/default reference: Tasmota support_command.ino and settings.ino,
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
*/

/** @file energy_precision.cpp
 * @brief Output-only resolution commands; all physical, dated and tariff accounts remain exact.
 */
#include "services/energy/energy_presentation.h"
#include "services/settings/settings.h"
#if USE_SENSORS
#include "drivers/sensors/sensor_service.h"
#endif
#include "core/command/command.h"
#include "support/text.h"
#include "support/number.h"

namespace newmota::energy {
CommandStatus dispatch_precision(
    const CommandRequest& request, Settings& settings, CommandOutput& output) {
    constexpr const char* names[]{
        "EnergyRes", "WattRes", "VoltRes", "AmpRes", "FreqRes", "WeightRes"};
    for (unsigned i = 0; i < 6; ++i) {
        if (!support::ascii_iequals(request.name, names[i]))
            continue;
        auto value = settings.energy_output_settings();
        if (!request.is_query()) {
            std::uint32_t resolution{};
            /** @brief Source setters ignore an out-of-range payload and return retained precision.
             */
            const std::uint32_t maximum = i ? 3 : 5;
            if (support::parse_unsigned_decimal(request.payload, UINT32_MAX, resolution) &&
                resolution <= maximum) {
                value.decimals[i] = resolution;
                const auto result = settings.set_energy_output_settings(value);
                if (result == SettingsUpdateResult::failed ||
                    result == SettingsUpdateResult::invalid)
                    return CommandStatus::failed;
#if USE_SENSORS
                // Sensor cards share the energy decimals, as Tasmota's sensors do.
                sensor_service().set_presentation(settings.sensor_presentation());
#endif
            }
        }
        return output.format(
                   "{\"%s\":%u}", names[i], unsigned(settings.energy_output_settings().decimals[i]))
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    }
    return CommandStatus::unknown;
}
} // namespace newmota::energy
