/*
  energy_calibration_learning.cpp - Retained device-learned common energy coefficients

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Chip-default latching reference: Tasmota xnrg_02_cse7766.ino,
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
*/

/** @file energy_calibration_learning.cpp
 * @brief Checkpoints already-applied common calibration without recursive driver transactions.
 */
#include "services/energy/energy_service.h"

namespace newmota::energy {
void Service::capture_calibration(Provider& instance) {
    if (instance.device->independent_calibration())
        return;
    Calibration value{};
    if (!instance.device->learned_calibration(value))
        return;
    auto& previous = instance.configuration.calibration;
    if (value.power == previous.power && value.voltage == previous.voltage &&
        value.current == previous.current && value.frequency == previous.frequency)
        return;
    /** The device already applies this exact snapshot. Capturing it never recalibrates,
     * emits measured energy, clears CF/counter baselines or changes existing account totals.
     * The normal dirty checkpoint/save-error path owns durability; failure retains live state.
     */
    previous = value;
    for (std::size_t c = 0; c < instance.configuration.channels; ++c)
        instance.channels[c].calibration = value;
    dirty_ = true;
}
} // namespace newmota::energy
