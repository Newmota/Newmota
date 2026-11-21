/*
  tuya_sensors.cpp - TuyaMCU sensors as a sensor driver

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file tuya_sensors.cpp
 * @brief Shows the MCU's sensor data points as Tasmota's TuyaSNS; the TuyaMCU driver keeps them.
 */
#include "drivers/sensors/sensor_service.h"
#include "drivers/tuya/tuya_mcu.h"

namespace newmota {
namespace {
void reset() {}

void initialize(std::uint32_t, const Hardware*) {}

void poll(std::uint32_t) {}

bool visit_readings(SensorReadingVisitor visitor, void* context) {
    return tuya::visit_sensor_readings(visitor, context);
}
} // namespace

// Constant lifecycle descriptor: no static constructor or linker discovery.
extern const SensorDriverHooks tuya_sensor_driver{reset, initialize, poll, visit_readings};
} // namespace newmota
