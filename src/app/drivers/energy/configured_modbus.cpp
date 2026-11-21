/*
  configured_modbus.cpp - Runtime-configured Modbus energy driver

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file configured_modbus.cpp
 * @brief Creates independently configurable energy maps on the shared RTU owner.
 */

#include "drivers/energy/modbus_meter.h"

namespace newmota::energy {
/** @brief Creates the configured map-backed Modbus energy instance.
 * @param config Persisted identity, channel count, and bounded JSON register map.
 * @return Meter protocol owner, or nullptr when the map or shared RTU bus is unavailable.
 */
std::unique_ptr<Device> create_configured_modbus(const Configuration& config) {
    return registers::create_configured(config);
}
} // namespace newmota::energy
