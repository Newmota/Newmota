/*
  bme280.cpp - Optional BME280 and BMP280 register driver

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file bme280.cpp
 * @brief Optional BME280 and BMP280 register driver.
 */

#include "drivers/sensors/sensor_service.h"
#include "newmota/log.h"
#include "newmota/platform/i2c.h"
#include "support/math.h"
#include <cmath>
#include <cstdio>

namespace newmota {
namespace {
// Independently implemented from Bosch BST-BME280-DS001, compensation equations.
// No vendor driver source is linked. Trimming values are read per device.
struct Device {
    std::uint8_t bus{}, address{}, chip{};
    std::uint16_t t1{}, p1{};
    std::int16_t t2{}, t3{}, p[8]{}, h2{}, h4{}, h5{};
    std::uint8_t h1{}, h3{};
    std::int8_t h6{};
    std::int32_t temperature{}, pressure{}, humidity{};
    bool detected{}, valid{}, humidity_valid{};
};

std::array<Device, 4> devices{};
std::uint32_t initialized_at{};

bool read(const Device& d, std::uint8_t reg, std::uint8_t* data, std::size_t size) {
    return platform::i2c::transfer(d.bus, d.address, &reg, 1, data, size) ==
           platform::i2c::Result::success;
}

bool write(const Device& d, std::uint8_t reg, std::uint8_t value) {
    const std::uint8_t data[]{reg, value};
    return platform::i2c::transfer(d.bus, d.address, data, 2, nullptr, 0) ==
           platform::i2c::Result::success;
}

std::uint16_t word(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(data[0] | (unsigned(data[1]) << 8));
}

std::int16_t signed_word(const std::uint8_t* data) {
    const auto value = word(data);
    return static_cast<std::int16_t>(value < 32768 ? value : int(value) - 65536);
}

std::int16_t signed12(unsigned value) {
    return static_cast<std::int16_t>(value < 2048 ? int(value) : int(value) - 4096);
}

constexpr std::uint8_t kCtrlMeas = 0x27; // Normal mode, temperature and pressure x1.

bool configure(const Device& d) {
    if (d.chip == 0x60 && !write(d, 0xf2, 1))
        return false; // Humidity x1 before ctrl_meas latches it.
    // Sleep before config, 1000 ms standby, filter off, normal mode x1/x1.
    return write(d, 0xf4, 0) && write(d, 0xf5, 0xa0) && write(d, 0xf4, kCtrlMeas);
}

void reset() {
    devices = {};
}

void initialize(std::uint32_t now, const Hardware*) {
    initialized_at = now;
    std::size_t i = 0;
    for (std::uint8_t bus = 0; bus < 2; ++bus)
        for (std::uint8_t address : {std::uint8_t{0x76}, std::uint8_t{0x77}}) {
            auto& d = devices[i++];
            d = {};
            d.bus = bus;
            d.address = address;
            std::uint8_t calibration[26]{}, status{};
            if (!read(d, 0xd0, &d.chip, 1) || (d.chip != 0x60 && d.chip != 0x58) ||
                !read(d, 0xf3, &status, 1) || (status & 9) ||
                !read(d, 0x88, calibration, d.chip == 0x60 ? 26 : 24))
                continue;
            d.t1 = word(calibration);
            d.t2 = signed_word(calibration + 2);
            d.t3 = signed_word(calibration + 4);
            d.p1 = word(calibration + 6);
            for (unsigned n = 0; n < 8; ++n)
                d.p[n] = signed_word(calibration + 8 + n * 2);
            if (d.t1 == 0 || d.t1 == 0xffff || d.p1 == 0 || d.p1 == 0xffff)
                continue;
            if (d.chip == 0x60) {
                d.h1 = calibration[25];
                std::uint8_t h[7]{};
                if (!read(d, 0xe1, h, sizeof h))
                    continue;
                d.h2 = signed_word(h);
                d.h3 = h[2];
                d.h4 = signed12((unsigned(h[3]) << 4) | (h[4] & 15));
                d.h5 = signed12((unsigned(h[5]) << 4) | (h[4] >> 4));
                d.h6 = static_cast<std::int8_t>(h[6] < 128 ? int(h[6]) : int(h[6]) - 256);
            }
            d.detected = configure(d);
        }
}

void poll(std::uint32_t now) {
    if (now - initialized_at < 20)
        return; // x1 all-field conversion bounded by datasheet.
    for (auto& d : devices) {
        if (!d.detected)
            continue;
        d.valid = d.humidity_valid = false;
        std::uint8_t status[2]{}, data[8]{}; // status, ctrl_meas
        if (!read(d, 0xf3, status, sizeof status))
            continue;
        if (status[1] != kCtrlMeas) {
            // A supply glitch resets the chip to sleep mode; the trimming values survive.
            AddLog(LogLevel::info, "BMP: %s-%02X lost config (ctrl_meas %02X), re-init",
                d.chip == 0x60 ? "BME280" : "BMP280", d.address, status[1]);
            (void)configure(d);
            continue;
        }
        if ((status[0] & 9) || !read(d, 0xf7, data, d.chip == 0x60 ? 8 : 6))
            continue;
        const unsigned raw_p =
            (unsigned(data[0]) << 12) | (unsigned(data[1]) << 4) | (data[2] >> 4);
        const unsigned raw_t =
            (unsigned(data[3]) << 12) | (unsigned(data[4]) << 4) | (data[5] >> 4);
        if (raw_t == 0x80000 || raw_p == 0x80000)
            continue;
        const double delta = raw_t / 16384.0 - d.t1 / 1024.0;
        const double curve = raw_t / 131072.0 - d.t1 / 8192.0;
        const double fine = delta * d.t2 + curve * curve * d.t3;
        const double temperature = fine / 5120.0;
        double a = fine / 2.0 - 64000.0;
        double b = a * a * d.p[4] / 32768.0 + a * d.p[3] * 2.0;
        b = b / 4.0 + d.p[2] * 65536.0;
        a = (d.p[1] * a * a / 524288.0 + d.p[0] * a) / 524288.0;
        a = (1.0 + a / 32768.0) * d.p1;
        if (a == 0)
            continue;
        double pressure = (1048576.0 - raw_p - b / 4096.0) * 6250.0 / a;
        pressure +=
            (d.p[7] * pressure * pressure / 2147483648.0 + pressure * d.p[6] / 32768.0 + d.p[5]) /
            16.0;
        if (!std::isfinite(temperature) || !std::isfinite(pressure) || temperature < -40 ||
            temperature > 85 || pressure < 30000 || pressure > 110000)
            continue;
        d.temperature = static_cast<std::int32_t>(::lround(temperature * 1000));
        d.pressure = static_cast<std::int32_t>(::lround(pressure * 10)); // Pa -> milli-hPa.
        d.valid = true;
        if (d.chip == 0x60) {
            const unsigned raw_h = (unsigned(data[6]) << 8) | data[7];
            if (raw_h == 0x8000)
                continue;
            double humidity = fine - 76800.0;
            humidity =
                (raw_h - (d.h4 * 64.0 + d.h5 * humidity / 16384.0)) *
                (d.h2 / 65536.0 *
                    (1.0 + d.h6 * humidity / 67108864.0 * (1.0 + d.h3 * humidity / 67108864.0)));
            humidity *= 1.0 - d.h1 * humidity / 524288.0;
            if (!std::isfinite(humidity))
                continue;
            humidity = math::fmin(100.0, math::fmax(0.0, humidity));
            d.humidity = static_cast<std::int32_t>(::lround(humidity * 1000));
            d.humidity_valid = true;
        }
    }
}

bool visit_readings(SensorReadingVisitor visitor, void* context) {
    unsigned count = 0;
    for (const auto& d : devices)
        if (d.detected)
            ++count;
    for (const auto& d : devices) {
        if (!d.valid)
            continue;
        char name[20]{};
        const char* type = d.chip == 0x60 ? "BME280" : "BMP280";
        if (count == 1)
            std::snprintf(name, sizeof name, "%s", type);
        else
            std::snprintf(name, sizeof name, "%s-%02X%s", type, d.address, d.bus ? "-2" : "");
        if (!visitor(context, {name, "Temperature", SensorQuantity::temperature, d.temperature}) ||
            !visitor(context, {name, "Pressure", SensorQuantity::pressure, d.pressure}) ||
            (d.humidity_valid &&
                !visitor(context, {name, "Humidity", SensorQuantity::humidity, d.humidity})))
            return false;
    }
    return true;
}
} // namespace

extern const SensorDriverHooks bme280_driver{reset, initialize, poll, visit_readings};
} // namespace newmota
