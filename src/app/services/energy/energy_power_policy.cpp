/*
  energy_power_policy.cpp - Primary source power observation policy

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Power-on reference: Tasmota xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later.
*/

/** @file energy_power_policy.cpp
 * @brief Observes global Power/SetOption21 for opted-in primary drivers, without relay claims.
 */
#include "services/energy/energy_service.h"

namespace newmota::energy {
void Service::apply_primary_power() {
    auto& device = provider_.device;
    if (device) {
        /** Missing binding retains the source initialization state (power_on=true).
         * Each opted-in driver implements its own OFF publication/accounting behavior;
         * the shared owner cannot safely zero all samples or discard physical counters.
         */
        const bool on = !primary_power_ || primary_power_(primary_power_context_);
        (void)device->set_primary_power(on);
#if USE_ENERGY_DUMMY
        device->set_primary_power_mask(
            primary_power_mask_ ? primary_power_mask_(primary_power_mask_context_) : 0);
#endif
    }
}
} // namespace newmota::energy
