/*
  dht.cpp - Portable DHT11, DHT22, and SI7021 pulse reader

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file dht.cpp
 * @brief Portable DHT11, DHT22, and SI7021 pulse reader.
 */

#include "drivers/sensors/dht_protocol.h"
#include "drivers/sensors/sensor_service.h"
#include "core/hardware/hardware.h"
#include "newmota/platform/gpio.h"
#include "newmota/platform/platform.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace newmota {
namespace {
constexpr std::uint32_t kPollIntervalMs = 4000U;
constexpr std::uint32_t kEdgeTimeoutMs = 2U;
constexpr std::uint32_t kBitSampleDelayUs = 45U;

struct Device {
    PinId pin{};
    DhtModel model{DhtModel::dht11};
    const char* name{};
    std::uint32_t next_read{};
    std::int32_t temperature{};
    std::int32_t humidity{};
    std::int32_t dew_point{};
    bool configured{};
    bool valid{};
};

std::array<Device, 3> devices{};

bool wait_level(const PinId pin, const bool expected) {
    const auto started = platform::monotonic_milliseconds();
    do {
        bool high = false;
        if (platform::gpio_read(pin, high) != platform::GpioResult::success)
            return false;
        if (high == expected)
            return true;
    } while (platform::monotonic_milliseconds() - started < kEdgeTimeoutMs);
    return false;
}

bool read_bit(const PinId pin, bool& value) {
    if (!wait_level(pin, true))
        return false;
    // A zero falls after 26–28 us; a one stays high for about 70 us. Sample
    // between them using the native busy wait, rather than counting GPIO reads.
    // The millisecond clock only bounds missing edges; it cannot classify bits.
    platform::delay_microseconds(kBitSampleDelayUs);
    if (platform::gpio_read(pin, value) != platform::GpioResult::success)
        return false;
    return !value || wait_level(pin, false);
}

bool capture_frame(const Device& device, std::uint8_t (&frame)[5]) {
    const auto timing = dht_timing(device.model);
    if (!timing.start_low_microseconds)
        return false;
    if (platform::gpio_configure_input(device.pin, platform::GpioPull::up) !=
        platform::GpioResult::success)
        return false;
    platform::delay_microseconds(1000U);
    if (platform::gpio_configure_output(device.pin, false) != platform::GpioResult::success)
        return false;
    platform::delay_microseconds(timing.start_low_microseconds);
    if (platform::gpio_configure_input(device.pin, platform::GpioPull::up) !=
        platform::GpioResult::success) {
        (void)platform::gpio_configure_input(device.pin, platform::GpioPull::up);
        return false;
    }
    platform::delay_microseconds(timing.response_settle_microseconds);

    if (!wait_level(device.pin, false) || !wait_level(device.pin, true) ||
        !wait_level(device.pin, false))
        return false;

    for (unsigned bit = 0; bit < 40U; ++bit) {
        bool one = false;
        if (!read_bit(device.pin, one))
            return false;
        frame[bit / 8U] = static_cast<std::uint8_t>(
            (static_cast<std::uint32_t>(frame[bit / 8U]) << 1U) | (one ? 1U : 0U));
    }
    return true;
}

void reset() {
    for (auto& device : devices) {
        if (device.configured)
            (void)platform::gpio_configure_input(device.pin, platform::GpioPull::up);
        device = {};
    }
}

void initialize(const std::uint32_t now, const Hardware* hardware) {
    if (hardware == nullptr)
        return;
    constexpr std::array<PinRole, 3> roles{PinRole::dht11, PinRole::dht22, PinRole::si7021};
    constexpr std::array<const char*, 3> names{"DHT11", "DHT22", "SI7021"};
    for (std::size_t index = 0; index < devices.size(); ++index) {
        const auto* const binding = hardware->find_function(roles[index], 1U);
        if (binding == nullptr || !binding->active())
            continue;
        auto& device = devices[index];
        device.pin = binding->pin;
        device.model = static_cast<DhtModel>(index + 1U);
        device.name = names[index];
        device.next_read = now + kPollIntervalMs;
        device.configured = platform::gpio_configure_input(device.pin, platform::GpioPull::up) ==
                            platform::GpioResult::success;
    }
}

void poll(const std::uint32_t now) {
    for (auto& device : devices) {
        if (!device.configured || static_cast<std::int32_t>(now - device.next_read) < 0)
            continue;
        device.next_read = now + kPollIntervalMs;
        device.valid = false;
        std::uint8_t frame[5]{};
        std::int32_t temperature = 0;
        std::int32_t humidity = 0;
        std::int32_t dew_point = 0;
        if (!capture_frame(device, frame) ||
            !decode_dht_frame(device.model, frame, temperature, humidity))
            continue;
        if (humidity == 0)
            humidity = 100; // Tasmota's lower sensor boundary is 0.1%.
        if (!dht_dew_point_milli_celsius(temperature, humidity, dew_point))
            continue;
        device.temperature = temperature;
        device.humidity = humidity;
        device.dew_point = dew_point;
        device.valid = true;
    }
}

bool visit_readings(const SensorReadingVisitor visitor, void* context) {
    constexpr std::array<const char*, 3> fields{"Temperature", "Humidity", "DewPoint"};
    for (const auto& device : devices) {
        if (!device.configured || !device.valid)
            continue;
        const SensorReading readings[]{
            {device.name, fields[0], SensorQuantity::temperature, device.temperature},
            {device.name, fields[1], SensorQuantity::humidity, device.humidity},
            {device.name, fields[2], SensorQuantity::dew_point, device.dew_point}};
        for (const auto& reading : readings)
            if (!visitor(context, reading))
                return false;
    }
    return true;
}

} // namespace

extern const SensorDriverHooks dht_driver{reset, initialize, poll, visit_readings};
} // namespace newmota
