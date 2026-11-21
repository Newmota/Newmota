/*
  dht_protocol.h - DHT frame decoding helpers

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#pragma once

#include <cmath>
#include <cstdint>

namespace newmota {

enum class DhtModel : std::uint8_t { dht11 = 1, dht22 = 2, si7021 = 3 };

struct DhtTiming {
    std::uint32_t start_low_microseconds;
    std::uint32_t response_settle_microseconds;
};

/** @brief Returns the model-specific host start and response timing used by Tasmota. */
[[nodiscard]] constexpr DhtTiming dht_timing(const DhtModel model) {
    switch (model) {
    case DhtModel::dht11:
        return {19000U, 50U};
    case DhtModel::dht22:
        return {2000U, 50U};
    case DhtModel::si7021:
        return {400U, 30U};
    }
    return {0U, 0U};
}

/** @brief Decodes a checksummed five-byte DHT frame into canonical milli-units. */
[[nodiscard]] inline bool decode_dht_frame(const DhtModel model, const std::uint8_t* frame,
    std::int32_t& temperature_milli_celsius, std::int32_t& humidity_milli_percent) {
    if (frame == nullptr ||
        static_cast<std::uint8_t>(frame[0] + frame[1] + frame[2] + frame[3]) != frame[4])
        return false;

    std::int32_t temperature_tenths = 0;
    std::int32_t humidity_tenths = 0;
    if (model == DhtModel::dht11) {
        humidity_tenths =
            static_cast<std::int32_t>(frame[0]) * 10 + static_cast<std::int32_t>(frame[1]);
        const auto fractional_tenths = static_cast<std::int32_t>(frame[3] & 0x0fU);
        temperature_tenths = static_cast<std::int32_t>(frame[2]) * 10 + fractional_tenths;
        if ((frame[3] & 0x80U) != 0U)
            temperature_tenths =
                -((static_cast<std::int32_t>(frame[2]) + 1) * 10 - fractional_tenths);
        if (temperature_tenths < -400 || temperature_tenths > 800)
            return false;
    } else if (model == DhtModel::dht22 || model == DhtModel::si7021) {
        humidity_tenths = (static_cast<std::int32_t>(frame[0]) << 8) | frame[1];
        const auto encoded_temperature =
            (static_cast<std::uint32_t>(frame[2]) << 8U) | static_cast<std::uint32_t>(frame[3]);
        const auto magnitude = static_cast<std::int32_t>(encoded_temperature & 0x7fffU);
        temperature_tenths = (encoded_temperature & 0x8000U) != 0U ? -magnitude : magnitude;
        if (temperature_tenths < -400 || temperature_tenths > 800)
            return false;
    } else {
        return false;
    }
    if (humidity_tenths < 0 || humidity_tenths > 1000)
        return false;

    temperature_milli_celsius = temperature_tenths * 100;
    humidity_milli_percent = humidity_tenths * 100;
    return true;
}

/** @brief Calculates Magnus dew point from canonical temperature and humidity. */
[[nodiscard]] inline bool dht_dew_point_milli_celsius(const std::int32_t temperature_milli_celsius,
    const std::int32_t humidity_milli_percent, std::int32_t& dew_point_milli_celsius) {
    if (humidity_milli_percent <= 0 || humidity_milli_percent > 100000 ||
        temperature_milli_celsius < -40000 || temperature_milli_celsius > 80000)
        return false;
    constexpr double a = 17.62;
    constexpr double b = 243.5;
    const double temperature = static_cast<double>(temperature_milli_celsius) / 1000.0;
    const double humidity = static_cast<double>(humidity_milli_percent) / 100000.0;
    const double gamma = std::log(humidity) + a * temperature / (b + temperature);
    const double dew_point = b * gamma / (a - gamma);
    if (!std::isfinite(dew_point) || dew_point < -100.0 || dew_point > 100.0)
        return false;
    dew_point_milli_celsius = static_cast<std::int32_t>(::lround(dew_point * 1000.0));
    return true;
}

} // namespace newmota
