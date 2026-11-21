/*
  bh1750.cpp - owned I2C and optional driver support for Newmota

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
std::array<SensorSample, 4> samples{};
std::uint32_t initialized_at{};

/** @brief Clears discovery and current output without touching the bus. */
void reset() {
    samples = {};
}

/** @brief Starts default continuous measurements at the selected device addresses. */
void initialize(std::uint32_t now, const Hardware*) {
    initialized_at = now;
    std::size_t i = 0;
    for (std::uint8_t bus = 0; bus < 2; ++bus)
        for (std::uint8_t address : {std::uint8_t{0x23}, std::uint8_t{0x5c}}) {
            auto& sample = samples[i++];
            sample = {bus, address};
            const std::uint8_t command = 0x10; // Continuous high resolution, default MTreg 69.
            sample.detected = platform::i2c::transfer(bus, address, &command, 1, nullptr, 0) ==
                              platform::i2c::Result::success;
        }
}

/** @brief Reads fresh illuminance after the maximum conversion interval. */
void poll(std::uint32_t now) {
    if (now - initialized_at < 180)
        return; // Maximum default conversion time.
    for (auto& sample : samples) {
        if (!sample.detected)
            continue;
        std::uint8_t data[2]{};
        sample.valid = platform::i2c::transfer(sample.bus, sample.address, nullptr, 0, data, 2) ==
                       platform::i2c::Result::success;
        if (sample.valid)
            sample.value = ((std::uint32_t(data[0]) << 8) | data[1]) * 10000 / 12;
    }
}

bool visit_readings(SensorReadingVisitor visitor, void* context) {
    unsigned detected = 0;
    for (const auto& sample : samples)
        if (sample.detected)
            ++detected;
    for (const auto& sample : samples) {
        if (!sample.valid)
            continue;
        char sensor[16]{};
        if (detected == 1) {
            std::snprintf(sensor, sizeof(sensor), "BH1750");
        } else {
            std::snprintf(sensor, sizeof(sensor), "BH1750-%02X%s", unsigned(sample.address),
                sample.bus == 0 ? "" : "-2");
        }
        if (!visitor(context, {sensor, "Illuminance", SensorQuantity::illuminance, sample.value}))
            return false;
    }
    return true;
}

} // namespace

// Constant lifecycle descriptor: no static constructor or linker discovery.
extern const SensorDriverHooks bh1750_driver{reset, initialize, poll, visit_readings};
} // namespace newmota
