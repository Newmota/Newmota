/*
  energy_protection.cpp - Source-compatible energy protection output routing

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Group RestorePower versus selected ON retry reference: Tasmota xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later.
*/

/** @file energy_protection.cpp
 * @brief Routes the selected provider through ordinary logical Power endpoints.
 */
#include "services/energy/energy_service.h"
#include "services/power/power_service.h"

namespace newmota::energy {
bool Service::protection_targets(
    std::size_t channel, std::uint32_t& selection, std::uint32_t& on) const {
    selection = 0;
    const auto& instance = provider_;
    if (!instance.channels || channel > instance.configuration.channels)
        return false;
    if (!protection_power_)
        return false;
    // Source group operations address all configured Power endpoints. Per-phase operations
    // use the corresponding logical Power index; a one-relay plug needs no extra mapping.
    if (channel)
        selection = std::uint32_t{1} << (channel - 1);
    else
        for (std::uint8_t index = 1; index <= 32; ++index) {
            bool powered{};
            if (protection_power_->state(index, powered))
                selection |= std::uint32_t{1} << (index - 1);
        }
    return protection_power_->target_state(selection, on);
}

PowerChangeResult Service::protection_trip(std::size_t channel, PowerOperationSource source) {
    std::uint32_t mask{}, on{};
    if (!protection_targets(channel, mask, on))
        return PowerChangeResult::unavailable;
    return channel ? protection_power_->force_off_selected(mask, source)
                   : protection_power_->force_off_group(mask, source);
}

PowerChangeResult Service::protection_retry(std::size_t channel, PowerOperationSource source) {
    std::uint32_t mask{}, on{};
    if (!protection_targets(channel, mask, on))
        return PowerChangeResult::unavailable;
    return channel ? protection_power_->set_selected(mask, PowerAction::on, source)
                   : protection_power_->restore_last_nonzero(mask, source);
}

} // namespace newmota::energy
