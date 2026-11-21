/*
  light_color.h - shared color transforms for Light commands and APIs

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file light_color.h
 * @brief Shared color transforms for Light commands and APIs.
 */

#pragma once

#include "services/light/light_settings.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace newmota::light_color {

/** @brief Interpolates the shared electrical gamma2.8 curve on0..65535. */
[[nodiscard]] std::uint16_t gamma16(std::uint16_t value);

/** @brief Scales a value between two unsigned ranges without floating point. */
[[nodiscard]] std::uint32_t scale(
    std::uint32_t value, std::uint32_t source_max, std::uint32_t target_max);
/** @brief Converts an 8-bit brightness value to a percentage. */
[[nodiscard]] std::uint8_t brightness_percent(std::uint8_t brightness);
/** @brief Converts a brightness percentage to the internal 8-bit value. */
[[nodiscard]] std::uint8_t brightness_value(std::uint8_t percent);
/** @brief Applies a percentage to one 8-bit color channel. */
[[nodiscard]] std::uint8_t effective_channel(std::uint8_t channel, std::uint8_t percent);
/** @brief Normalizes selected color channels and returns their effective brightness. */
[[nodiscard]] std::uint8_t normalize_channels(
    std::array<std::uint8_t, 5>& channels, std::size_t first, std::size_t count);
/** @brief Converts HSB input to three RGB channels. */
void hsb_to_rgb(std::uint16_t hue, std::uint8_t saturation, std::array<std::uint8_t, 3>& channels);
/** @brief Derives hue and saturation from the RGB channels in a five-channel array. */
void rgb_to_hs(
    const std::array<std::uint8_t, 5>& channels, std::uint16_t& hue, std::uint8_t& saturation);
/** @brief Selects the white-channel mix for the configured Light channel count. */
void select_white(std::uint8_t channel_count, LightSettings& settings, std::uint8_t brightness);
/** @brief Sets the configured color-temperature channel values. */
void set_color_temperature(
    std::uint8_t channel_count, bool separate, LightSettings& settings, std::uint16_t ct);
/** @brief Derives color-temperature bounds from the configured channel count. */
void derive_color_temperature(std::uint8_t channel_count, LightSettings& settings);

} // namespace newmota::light_color
