/*
  tc74.cpp - owned I2C and optional driver support for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "drivers/sensors/sensor_service.h"
#include "core/command/command.h"
#include "newmota/platform/i2c.h"
#include <cstdio>

namespace newmota {
namespace {
std::array<SensorSample, 16> samples{};

/** @brief Clears discovery and current output without touching the bus. */
void reset() {
    samples = {};
}

/** @brief Detects plausible TC74 devices and exits standby when necessary. */
void initialize(std::uint32_t, const Hardware*) {
    std::size_t i = 0;
    for (std::uint8_t bus = 0; bus < 2; ++bus)
        for (std::uint8_t address = 0x48; address <= 0x4f; ++address) {
            auto& sample = samples[i++];
            sample = {bus, address};
            const std::uint8_t reg = 1;
            std::uint8_t config{};
            sample.detected = platform::i2c::transfer(bus, address, &reg, 1, &config, 1) ==
                                  platform::i2c::Result::success &&
                              (config & 0x3f) == 0;
            // TC74 has no product ID. Require the documented reserved configuration bits.
            if (sample.detected && (config & 0x80)) {
                const std::uint8_t wake[2]{1, 0};
                sample.detected = platform::i2c::transfer(bus, address, wake, 2, nullptr, 0) ==
                                  platform::i2c::Result::success;
            }
        }
}

/** @brief Checks DATA_RDY and reads signed Celsius samples. */
void poll(std::uint32_t) {
    for (auto& sample : samples) {
        if (!sample.detected)
            continue;
        const std::uint8_t config_reg = 1, temperature_reg = 0;
        std::uint8_t config{}, temperature{};
        sample.valid = platform::i2c::transfer(sample.bus, sample.address, &config_reg, 1, &config,
                           1) == platform::i2c::Result::success &&
                       (config & 0xc0) == 0x40 && (config & 0x3f) == 0 &&
                       platform::i2c::transfer(sample.bus, sample.address, &temperature_reg, 1,
                           &temperature, 1) == platform::i2c::Result::success;
        if (sample.valid)
            sample.value = (temperature < 128 ? temperature : int(temperature) - 256) * 1000;
    }
}

bool visit_readings(SensorReadingVisitor visitor, void* context) {
    for (const auto& sample : samples) {
        if (!sample.valid)
            continue;
        char sensor[16]{};
        std::snprintf(sensor, sizeof(sensor), "TC74-%02X%s", unsigned(sample.address),
            sample.bus == 0 ? "" : "-2");
        if (!visitor(context, {sensor, "Temperature", SensorQuantity::temperature, sample.value}))
            return false;
    }
    return true;
}

} // namespace

// Constant lifecycle descriptor: no static constructor or linker discovery.
extern const SensorDriverHooks tc74_driver{reset, initialize, poll, visit_readings};
} // namespace newmota
