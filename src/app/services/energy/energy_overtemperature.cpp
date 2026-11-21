/*
  energy_overtemperature.cpp - Primary source global-temperature protection

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  EnergyEverySecond/use_overtemp reference: Tasmota xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later.
*/

/** @file energy_overtemperature.cpp
 * @brief Executes source global overtemperature policy without inventing meter/relay mappings.
 */
#include "services/energy/energy_service.h"
#include "newmota/log.h"

namespace newmota::energy {
void Service::poll_overtemperature(std::uint32_t now) {
    const auto second = now / 1000;
    if (second == overtemperature_second_)
        return;
    overtemperature_second_ = second;
    const auto& primary = provider_;
    if (!primary.device || !primary.device->use_overtemperature() || !overtemperature_read_ ||
        !overtemperature_off_)
        return;
    std::int32_t celsius{};
    std::uint8_t threshold{};
    if (overtemperature_read_(overtemperature_context_, now, celsius, threshold) &&
        std::int64_t(celsius) > std::int64_t(threshold) * 1000) {
        AddLog(LogLevel::debug, "NRG: global temperature exceeds SetOption42");
        if (!overtemperature_off_(overtemperature_context_))
            AddLog(LogLevel::error, "NRG: forced overtemperature output shutdown failed");
    }
}
} // namespace newmota::energy
