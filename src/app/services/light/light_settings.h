/*
  light_settings.h - portable Light behavior for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file light_settings.h
 * @brief Portable Light behavior for Newmota.
 */

#pragma once
#include "newmota_features.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace newmota {
inline constexpr std::size_t kMaximumCompatibilityPwmOutputs = 16U;

struct LightSettings final {
    std::uint32_t frequency{977U};
    std::uint16_t range{1023U};
    std::array<std::uint32_t, 16> channel_frequency{}; // Zero inherits global carrier.
    std::array<std::array<std::uint8_t, 5>, 8> palette{};
    std::uint8_t palette_count{};
    std::uint8_t palette_index{};
    std::uint8_t fixed_color_index{1U};
    std::uint8_t scheme{};
    std::uint8_t white_dimmer{100U};
    std::uint16_t wakeup_duration{60U};
#if USE_WS2812
    std::uint16_t strip_pixels{30U};
    std::uint16_t strip_rotation{};
    std::uint16_t strip_step_pixels{};
    std::array<std::uint8_t, 4> strip_width{{1U, 1U, 3U, 5U}};
    std::uint16_t strip_height{1U};
    std::uint8_t strip_type{};
    bool strip_reverse{};
    bool strip_alternate{};
    std::array<std::array<std::uint8_t, 5>, 4> strip_clock_colors{{
        {{255U, 0U, 255U, 0U, 0U}},
        {{0U, 255U, 0U, 0U, 0U}},
        {{255U, 0U, 0U, 0U, 0U}},
        {{0U, 0U, 0U, 0U, 0U}},
    }};
#else
    std::array<std::uint8_t, 35> strip_extension_raw{};
#endif
    bool strip_extension_present{};

    [[nodiscard]] std::uint32_t carrier(std::size_t index) const {
        return channel_frequency[index] == 0U ? frequency : channel_frequency[index];
    }

    std::array<std::uint16_t, kMaximumCompatibilityPwmOutputs> raw{};
    std::array<std::uint8_t, 5> channels{255U, 255U, 255U, 0U, 0U};
    std::array<std::uint8_t, 5> rgbww_table{255U, 255U, 255U, 255U, 255U}; // RGBWWTable scale.
    std::array<std::uint8_t, 2> led_current{{0xFFU, 0xFFU}}; // LED chip RGB, white; 0xFF default.
    std::uint16_t color_temperature{326U};
    std::uint8_t dimmer{100U};
    std::uint8_t speed{1U};
    std::uint8_t desired_power{};
    bool gamma{true};
    bool fade{};

    [[nodiscard]] bool valid() const {
        if (frequency < 2U || frequency > 50000U ||
            (range != 255U && range != 511U && range != 1023U) || dimmer > 100U ||
            white_dimmer > 100U || palette_count > palette.size() ||
            (palette_count == 0U ? palette_index != 0U : palette_index >= palette_count) ||
            fixed_color_index < 1U || fixed_color_index > 12U || (scheme > 14U) ||
            wakeup_duration == 0U || wakeup_duration > 3000U || desired_power > 31U ||
            speed == 0U || speed > 40U || color_temperature < 153U || color_temperature > 500U
#if USE_WS2812
            || strip_pixels == 0U || strip_pixels > 512U || strip_rotation >= strip_pixels ||
            strip_step_pixels > 255U || strip_width[0] > 4U || strip_width[1] > 31U ||
            strip_width[2] > 31U || strip_width[3] > 31U || strip_height == 0U ||
            strip_height > 32768U || strip_type > 31U)
#else
            || scheme > 4U)
#endif
            return false;
        for (const auto hz : channel_frequency)
            if (hz != 0U && (hz < 2U || hz > 50000U))
                return false;
        for (const auto duty : raw)
            if (duty > range)
                return false;
        return true;
    }
};
} // namespace newmota
