/*
  wifi_tx_power.cpp - Tasmota's WifiPower policy for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/network/wifi_tx_power.h"

#include "newmota/platform/network.h"
#include "services/settings/settings.h"

#include <cstring>

namespace newmota::wifi_tx_power {
namespace {

/** @brief Last power handed to the radio in tenths of a dBm; negative before the first. */
int applied = -1;
bool was_connected = false;

/** @brief Returns the fixed power a non-dynamic setting stands for. */
[[nodiscard]] std::uint8_t fixed_power(
    const std::uint8_t setting, const platform::WifiTxPowerProfile& profile) {
    return setting == Settings::kWifiPowerChipDefault ? profile.max_54g : setting;
}

/**
 * @brief Tasmota's dynamic power: enough for the AP to hear this station at the current PHY
 * mode's sensitivity plus 3 dB, assuming the AP sends with 20 dBm. Concept by ESPEasy (@TD-er).
 */
[[nodiscard]] std::uint8_t dynamic_power(const platform::WifiTxPowerProfile& profile) {
    const auto rssi = platform::station_rssi();
    if (!rssi)
        return profile.max_54g;
    platform::WifiLink link{};
    (void)platform::wifi_link(link);
    int maximum = profile.max_n;
    int threshold = profile.sensitivity_n;
    if (link.mode != nullptr && std::strcmp(link.mode, "11b") == 0) {
        maximum = profile.max_11b;
        threshold = profile.sensitivity_11b;
    } else if (link.mode != nullptr && std::strcmp(link.mode, "11g") == 0) {
        maximum = profile.max_54g;
        threshold = profile.sensitivity_54g;
    }
    threshold += 30;
    const int link_margin = *rssi * 10 - 200;
    const int minimum = link_margin < threshold ? threshold - link_margin : 0;
    return static_cast<std::uint8_t>(minimum < maximum ? minimum : maximum);
}

} // namespace

void apply(const Settings& settings) {
    const auto state = platform::wifi_state();
    if (state == platform::WifiState::disabled || state == platform::WifiState::initializing)
        return;
    const bool connected = state == platform::WifiState::connected;
    const bool new_connection = connected && !was_connected;
    was_connected = connected;
    const auto profile = platform::wifi_tx_power_profile();
    const auto setting = settings.wifi_power();
    const int wanted = setting == 0U ? dynamic_power(profile) : fixed_power(setting, profile);
    if (wanted == applied && !new_connection)
        return;
    if (platform::set_wifi_tx_power(static_cast<std::uint8_t>(wanted)) ==
        platform::NetworkResult::success)
        applied = wanted;
}

std::uint8_t saved_value(const std::uint32_t requested) {
    const auto maximum = platform::wifi_tx_power_profile().max_11b;
    return requested < maximum ? static_cast<std::uint8_t>(requested) : maximum;
}

std::uint8_t reported(const Settings& settings) {
    const auto setting = settings.wifi_power();
    if (setting != 0U)
        return fixed_power(setting, platform::wifi_tx_power_profile());
    return applied < 0 ? 0U : static_cast<std::uint8_t>(applied);
}

} // namespace newmota::wifi_tx_power
