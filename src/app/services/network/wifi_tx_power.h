/*
  wifi_tx_power.h - Tasmota's WifiPower policy for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file wifi_tx_power.h
 * @brief Fixed and RSSI-based Wi-Fi transmit power, as Tasmota's WifiPower selects them.
 */

#pragma once

#include <cstdint>

namespace newmota {

class Settings;

namespace wifi_tx_power {

/**
 * @brief Applies the saved power once Wi-Fi runs, when it changes and on each new connection.
 *
 * WifiPower 0 follows the station's RSSI. Without a link it uses the chip default, so a
 * reconnect never starts at minimum power.
 */
void apply(const Settings& settings);

/**
 * @brief Limits a requested power to the chip's range and keeps Tasmota's special values.
 * @param requested Power in tenths of a dBm: 0 selects dynamic, 10 the chip default.
 * @return The value to save.
 */
[[nodiscard]] std::uint8_t saved_value(std::uint32_t requested);

/**
 * @brief Returns the power WifiPower and Status 5 report, in tenths of a dBm: the setting
 * when fixed, the last value applied when dynamic.
 */
[[nodiscard]] std::uint8_t reported(const Settings& settings);

} // namespace wifi_tx_power
} // namespace newmota
