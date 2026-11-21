/*
  energy_sensor.cpp - The energy meter as a dashboard sensor

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file energy_sensor.cpp
 * @brief Shows the selected meter as one sensor card; the energy service polls it itself.
 */
#include "drivers/sensors/sensor_service.h"
#include "services/energy/energy_service.h"

namespace newmota {
namespace {
void reset() {}

void initialize(std::uint32_t, const Hardware*) {}

void poll(std::uint32_t) {}

bool visit_readings(SensorReadingVisitor visitor, void* context) {
    return energy::service().visit_sensor_readings(visitor, context);
}
} // namespace

// Constant lifecycle descriptor: no static constructor or linker discovery.
extern const SensorDriverHooks energy_sensor_driver{reset, initialize, poll, visit_readings};
} // namespace newmota
