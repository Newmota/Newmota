/*
  light_color.cpp - shared color transforms for Light commands and APIs

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file light_color.cpp
 * @brief Shared color transforms for Light commands and APIs.
 */

#include "services/light/light_color.h"

#include <algorithm>

namespace newmota::light_color {

// Integer interpolation of gamma 2.8, including exact static endpoints.
constexpr std::array<std::uint16_t, 33> gamma_table{0U, 4U, 28U, 87U, 194U, 362U, 604U, 930U, 1351U,
    1879U, 2524U, 3296U, 4205U, 5261U, 6475U, 7854U, 9410U, 11151U, 13086U, 15225U, 17577U, 20150U,
    22953U, 25995U, 29285U, 32831U, 36642U, 40726U, 45092U, 49747U, 54701U, 59961U, 65535U};

std::uint16_t gamma16(const std::uint16_t value) {
    if (value == 65535U)
        return value;
    const auto scaled = static_cast<std::uint32_t>(value) * 32U;
    const auto index = scaled / 65535U;
    const auto remainder = scaled % 65535U;
    return static_cast<std::uint16_t>(
        gamma_table[index] +
        (static_cast<std::uint32_t>(gamma_table[index + 1U] - gamma_table[index]) * remainder +
            32767U) /
            65535U);
}

std::uint32_t scale(
    const std::uint32_t value, const std::uint32_t source_max, const std::uint32_t target_max) {
    if (value == 0U || source_max == 0U)
        return 0U;
    if (value >= source_max)
        return target_max;
    if (target_max > source_max) {
        const auto numerator = value * target_max * 2U;
        return std::min(target_max, ((numerator / source_max) + 1U) / 2U);
    }
    const auto numerator = (value * 2U + 1U) * (target_max + 1U);
    return std::min(target_max, numerator / ((source_max + 1U) * 2U));
}

std::uint8_t brightness_percent(const std::uint8_t brightness) {
    if (brightness == 0U)
        return 0U;
    return static_cast<std::uint8_t>(std::max<std::uint32_t>(1U, scale(brightness, 255U, 100U)));
}

std::uint8_t brightness_value(const std::uint8_t percent) {
    return static_cast<std::uint8_t>(scale(percent, 100U, 255U));
}

std::uint8_t effective_channel(const std::uint8_t channel, const std::uint8_t percent) {
    return static_cast<std::uint8_t>((static_cast<unsigned>(channel) * percent + 50U) / 100U);
}

std::uint8_t normalize_channels(
    std::array<std::uint8_t, 5>& channels, const std::size_t first, const std::size_t count) {
    std::uint8_t peak = 0U;
    for (std::size_t i = 0U; i < count; ++i)
        peak = std::max(peak, channels[first + i]);
    if (peak == 0U)
        return 0U;
    for (std::size_t i = 0U; i < count; ++i)
        channels[first + i] = static_cast<std::uint8_t>(
            (static_cast<unsigned>(channels[first + i]) * 255U + peak / 2U) / peak);
    return brightness_percent(peak);
}

void hsb_to_rgb(const std::uint16_t input_hue, const std::uint8_t saturation,
    std::array<std::uint8_t, 3>& channels) {
    const auto hue = static_cast<std::uint32_t>(input_hue % 360U);
    std::uint32_t red = 255U, green = 255U, blue = 255U;
    if (saturation != 0U) {
        const auto sector = hue / 60U;
        const auto offset = hue % 60U;
        const auto q = 255U - scale(offset, 60U, saturation);
        const auto p = 255U - saturation;
        const auto t = 255U - scale(60U - offset, 60U, saturation);
        switch (sector) {
        case 0U:
            green = t;
            blue = p;
            break;
        case 1U:
            red = q;
            blue = p;
            break;
        case 2U:
            red = p;
            blue = t;
            break;
        case 3U:
            red = p;
            green = q;
            break;
        case 4U:
            red = t;
            green = p;
            break;
        default:
            green = p;
            blue = q;
            break;
        }
    }
    channels = {static_cast<std::uint8_t>(red), static_cast<std::uint8_t>(green),
        static_cast<std::uint8_t>(blue)};
}

void rgb_to_hs(
    const std::array<std::uint8_t, 5>& channels, std::uint16_t& hue, std::uint8_t& saturation) {
    const std::uint32_t red = channels[0];
    const std::uint32_t green = channels[1];
    const std::uint32_t blue = channels[2];
    const auto maximum = std::max({red, green, blue});
    const auto minimum = std::min({red, green, blue});
    const auto delta = maximum - minimum;
    hue = 0U;
    saturation = 0U;
    if (delta == 0U)
        return;
    saturation = static_cast<std::uint8_t>(scale(delta, maximum, 255U));
    std::uint32_t value = 0U;
    if (red == maximum) {
        value =
            green > blue ? scale(green - blue, delta, 60U) : 360U - scale(blue - green, delta, 60U);
    } else if (green == maximum) {
        value = blue > red ? 120U + scale(blue - red, delta, 60U)
                           : 120U - scale(red - blue, delta, 60U);
    } else {
        value = red > green ? 240U + scale(red - green, delta, 60U)
                            : 240U - scale(green - red, delta, 60U);
    }
    hue = static_cast<std::uint16_t>(value % 360U);
}

void select_white(
    const std::uint8_t channel_count, LightSettings& settings, const std::uint8_t brightness) {
    if (channel_count == 4U) {
        settings.channels.fill(0U);
        settings.channels[3] = 255U;
    } else {
        const auto offset = channel_count == 2U ? 0U : 3U;
        const auto cold = settings.channels[offset];
        const auto warm = settings.channels[offset + 1U];
        const auto sum = static_cast<unsigned>(cold) + warm;
        std::uint8_t normalized_cold = 0U;
        std::uint8_t normalized_warm = 0U;
        if (sum == 0U) {
            const auto warm_from_ct = (settings.color_temperature - 153U) * 255U / 347U;
            normalized_warm = static_cast<std::uint8_t>(warm_from_ct);
            normalized_cold = static_cast<std::uint8_t>(255U - warm_from_ct);
        } else {
            const auto cold_from_mix = (static_cast<unsigned>(cold) * 255U + sum / 2U) / sum;
            normalized_cold = static_cast<std::uint8_t>(cold_from_mix);
            normalized_warm = static_cast<std::uint8_t>(255U - cold_from_mix);
        }
        settings.channels.fill(0U);
        settings.channels[offset] = normalized_cold;
        settings.channels[offset + 1U] = normalized_warm;
    }
    settings.dimmer = brightness;
}

void set_color_temperature(const std::uint8_t channel_count, const bool separate,
    LightSettings& settings, const std::uint16_t ct) {
    settings.color_temperature = ct;
    const auto offset = channel_count == 2U ? 0U : 3U;
    const auto warm = (ct - 153U) * 255U / 347U;
    if (!separate)
        settings.channels.fill(0U);
    settings.channels[offset] = static_cast<std::uint8_t>(255U - warm);
    settings.channels[offset + 1U] = static_cast<std::uint8_t>(warm);
}

void derive_color_temperature(const std::uint8_t channel_count, LightSettings& settings) {
    if (channel_count != 2U && channel_count != 5U)
        return;
    const auto offset = channel_count == 2U ? 0U : 3U;
    const auto sum =
        static_cast<unsigned>(settings.channels[offset]) + settings.channels[offset + 1U];
    if (sum != 0U)
        settings.color_temperature = static_cast<std::uint16_t>(
            153U + (347U * settings.channels[offset + 1U] + sum / 2U) / sum);
}

} // namespace newmota::light_color
