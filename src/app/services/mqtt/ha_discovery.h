/*
  ha_discovery.h - Home Assistant MQTT discovery for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file ha_discovery.h
 * @brief Publishes one retained Home Assistant discovery message per entity (plan 44).
 */

#pragma once

#include <cstdint>
#include <string_view>

namespace newmota {

class MqttService;
class PowerService;
class Settings;

namespace ha_discovery {

/** @brief Discovery prefix, Home Assistant's default. */
inline constexpr std::string_view kPrefix = "homeassistant";

/**
 * @brief Publishes, updates and removes the discovery messages while MQTT is connected.
 *
 * Runs after every connect and whenever the entity set or the device identity changes;
 * `SetOption19 1` removes every published entity. Sends at most one message per call.
 * @param model Module or template name shown as the device model.
 */
void poll(MqttService& mqtt, const Settings& settings, const PowerService& power,
    std::string_view model, std::uint32_t now);

} // namespace ha_discovery
} // namespace newmota
