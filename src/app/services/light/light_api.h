/*
  light_api.h - typed owner API for Light state and mutations

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file light_api.h
 * @brief Typed Light mutations with explicit caller provenance.
 */
#pragma once

#include "services/light/light_settings.h"
#include "services/power/power_service.h"

#include <array>
#include <cstdint>

namespace newmota {
class LightService;

struct LightApiSnapshot final {
    bool power{};
    bool applied_power{};
    std::uint8_t brightness{};
    std::uint16_t hue{};
    std::uint8_t saturation{};
    std::uint16_t color_temperature{};
    std::array<char, 7> rgb{};
    std::array<std::uint8_t, 5> channels{};
    std::uint8_t channel_count{};
    bool has_rgb{};
    bool has_color_temperature{};
    bool has_color_mode{};
    bool color_mode_rgb{};
};

struct LightApiMutation final {
    bool set_power{};
    bool power{};
    bool set_brightness{};
    std::uint8_t brightness{};
    bool set_hue{};
    std::uint16_t hue{};
    bool set_saturation{};
    std::uint8_t saturation{};
    bool set_color_temperature{};
    std::uint16_t color_temperature{};
    bool set_rgb{};
    std::array<std::uint8_t, 3> rgb{};
    bool set_channels{};
    std::array<std::uint8_t, 5> channels{};
    std::uint8_t channel_count{};
};

enum class LightApiError : std::uint8_t {
    none,
    unavailable,
    invalid_endpoint,
    invalid_request,
    hardware_failure,
};

/** @brief Reads one zero-based Light endpoint into its public value shape. */
[[nodiscard]] LightApiError light_api_get(
    const LightService& light, std::uint8_t endpoint, LightApiSnapshot& snapshot);

/** @brief Validates, applies, and reads back one zero-based endpoint update.
 * @param light Configured Light owner.
 * @param endpoint Zero-based endpoint.
 * @param mutation Requested typed field changes.
 * @param snapshot Receives resulting state.
 * @param source Caller origin for explicit power operations; linked changes use Light.
 * @return Validation or owner-operation result.
 */
[[nodiscard]] LightApiError light_api_set(LightService& light, std::uint8_t endpoint,
    const LightApiMutation& mutation, LightApiSnapshot& snapshot,
    PowerOperationSource source = PowerOperationSource::light);

} // namespace newmota
