/*
  energy_status.cpp - Primary retained energy reporting settings

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  EnergyMarginStatus/ResponseAppendMargin reference: Tasmota xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later.
*/

/** @file energy_status.cpp
 * @brief Projects primary phase settings without inventing unimplemented protection values.
 */
#include "services/energy/energy_service.h"
#include "core/command/command.h"

namespace newmota::energy {
bool Service::write_margin_status(CommandOutput& output) const {
    const auto& primary = provider_;
    if (!output.write("\"StatusPTH\":{"))
        return false;
    if (primary.channels && primary.configuration.channels) {
        constexpr const char* names[]{"PowerDelta", "PowerLow", "PowerHigh", "VoltageLow",
            "VoltageHigh", "CurrentLow", "CurrentHigh"
#if USE_ENERGY_MAX_POWER
            ,
            "MaxPower", "MaxPowerHold", "MaxPowerWindow"
#endif
#if USE_ENERGY_MAX_ENERGY
            ,
            "MaxEnergy", "MaxEnergyStart"
#endif
        };
        const auto count = primary.configuration.channels;
        for (unsigned field = 0; field < sizeof(names) / sizeof(names[0]); ++field) {
            if (!output.format("%s\"%s\":", field ? "," : "", names[field]) ||
                (count > 1 && !output.write("[")))
                return false;
            for (std::size_t channel = 0; channel < count; ++channel) {
                const auto& phase = primary.channels[channel];
                const auto energy_field = 7U + (USE_ENERGY_MAX_POWER ? 3U : 0U);
                const auto value = field >= energy_field ? phase.max_energy[field - energy_field]
                                   : field >= 7          ? phase.max_power[field - 7]
                                   : field               ? phase.margins[field - 1]
                                                         : phase.power_delta;
                if (!output.format("%s%u", channel ? "," : "", unsigned(value)))
                    return false;
            }
            if (count > 1 && !output.write("]"))
                return false;
        }
    }
    return output.write("}");
}
} // namespace newmota::energy
