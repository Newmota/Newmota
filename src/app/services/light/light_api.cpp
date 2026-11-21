/*
  light_api.cpp - typed owner API for Light state and mutations

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Berry explicit-power source reference: Tasmota xdrv_52_3_berry_light.ino,
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
*/

/** @file light_api.cpp
 * @brief Typed owner API for Light state and mutations.
 */

#include "services/light/light_api.h"

#include "services/light/light_color.h"
#include "services/light/light_service.h"

#include <algorithm>

namespace newmota {
namespace {

struct EndpointChannels final {
    std::uint8_t first{};
    std::uint8_t count{};
};

EndpointChannels endpoint_channels(const LightService& light, const std::uint8_t endpoint) {
    if (light.independent())
        return {endpoint, 1U};
    if (light.separate())
        return endpoint == 0U
                   ? EndpointChannels{0U, 3U}
                   : EndpointChannels{3U, static_cast<std::uint8_t>(light.channel_count() - 3U)};
    return {0U, light.channel_count()};
}

std::uint8_t dimmer_for_endpoint(
    const LightService& light, const std::uint8_t endpoint, const LightSettings& settings) {
    if (light.independent())
        return settings.dimmer;
    return light.separate() && endpoint == 1U ? settings.white_dimmer : settings.dimmer;
}

void set_dimmer_for_endpoint(const LightService& light, const std::uint8_t endpoint,
    LightSettings& settings, const std::uint8_t dimmer) {
    if (light.separate() && endpoint == 1U)
        settings.white_dimmer = dimmer;
    else
        settings.dimmer = dimmer;
}

bool endpoint_has_rgb(const LightService& light, const std::uint8_t endpoint) {
    return !light.independent() &&
           (light.separate() ? endpoint == 0U : light.channel_count() >= 3U);
}

bool endpoint_has_ct(const LightService& light, const std::uint8_t endpoint) {
    if (light.independent())
        return false;
    if (light.separate())
        return endpoint == 1U && light.channel_count() == 5U;
    return light.channel_count() == 2U || light.channel_count() == 5U;
}

std::uint8_t selected_channel(const LightService& light, const std::uint8_t endpoint) {
    if (light.independent())
        return static_cast<std::uint8_t>(endpoint + 1U);
    if (light.separate())
        return endpoint == 0U ? 1U : 4U;
    return 0U;
}

void materialize_independent_channels(const LightService& light, LightSettings& settings) {
    for (std::size_t i = 0U; i < light.channel_count(); ++i)
        settings.channels[i] =
            light_color::effective_channel(settings.channels[i], settings.dimmer);
    settings.dimmer = 100U;
}

std::uint8_t normalize_rgb(
    const std::array<std::uint8_t, 3>& rgb, LightSettings& settings, const bool clear_white) {
    const auto peak = std::max({rgb[0], rgb[1], rgb[2]});
    settings.channels[0] = rgb[0];
    settings.channels[1] = rgb[1];
    settings.channels[2] = rgb[2];
    if (clear_white) {
        settings.channels[3] = 0U;
        settings.channels[4] = 0U;
    }
    if (peak == 0U)
        return 0U;
    for (std::size_t i = 0U; i < rgb.size(); ++i)
        settings.channels[i] = static_cast<std::uint8_t>(
            (static_cast<unsigned>(settings.channels[i]) * 255U + peak / 2U) / peak);
    return light_color::brightness_percent(peak);
}

LightApiError validate(
    const LightService& light, const std::uint8_t endpoint, const LightApiMutation& mutation) {
    if (!light.available())
        return LightApiError::unavailable;
    if (endpoint >= light.endpoint_count())
        return LightApiError::invalid_endpoint;
    if (mutation.set_hue && mutation.hue > 360U)
        return LightApiError::invalid_request;
    if (mutation.set_color_temperature &&
        (mutation.color_temperature < 153U || mutation.color_temperature > 500U))
        return LightApiError::invalid_request;
    if ((mutation.set_hue || mutation.set_saturation || mutation.set_rgb) &&
        !endpoint_has_rgb(light, endpoint))
        return LightApiError::invalid_request;
    if (mutation.set_color_temperature && !endpoint_has_ct(light, endpoint))
        return LightApiError::invalid_request;
    if (mutation.set_channels && mutation.channel_count != endpoint_channels(light, endpoint).count)
        return LightApiError::invalid_request;
    return LightApiError::none;
}

} // namespace

LightApiError light_api_get(
    const LightService& light, const std::uint8_t endpoint, LightApiSnapshot& snapshot) {
    if (!light.available())
        return LightApiError::unavailable;
    if (endpoint >= light.endpoint_count())
        return LightApiError::invalid_endpoint;

    snapshot = {};
    const auto& settings = light.intent();
    const auto selected = endpoint_channels(light, endpoint);
    const auto dimmer = dimmer_for_endpoint(light, endpoint, settings);
    const auto desired_bit = static_cast<std::uint8_t>(1U << endpoint);
    snapshot.power = (settings.desired_power & desired_bit) != 0U;
    bool applied = false;
    snapshot.applied_power =
        light.state(static_cast<std::uint8_t>(endpoint + 1U), applied) && applied;
    snapshot.brightness = light.independent() ? light_color::effective_channel(
                                                    settings.channels[selected.first], dimmer)
                                              : light_color::brightness_value(dimmer);
    snapshot.channel_count = selected.count;
    snapshot.has_rgb = endpoint_has_rgb(light, endpoint);
    snapshot.has_color_temperature = endpoint_has_ct(light, endpoint);
    snapshot.color_temperature = settings.color_temperature;
    // A linked light shows colour or white, because setting one clears the other.
    snapshot.has_color_mode =
        !light.independent() && !light.separate() && light.channel_count() >= 4U;
    snapshot.color_mode_rgb =
        (settings.channels[0] | settings.channels[1] | settings.channels[2]) != 0U ||
        (settings.channels[3] | settings.channels[4]) == 0U;
    for (std::size_t i = 0U; i < selected.count; ++i) {
        const auto global = static_cast<std::size_t>(selected.first) + i;
        snapshot.channels[i] = light_color::effective_channel(settings.channels[global], dimmer);
    }
    if (snapshot.has_rgb) {
        std::array<std::uint8_t, 5> rgb_channels = settings.channels;
        std::uint8_t saturation = 0U;
        light_color::rgb_to_hs(rgb_channels, snapshot.hue, saturation);
        snapshot.saturation = saturation;
        for (std::size_t i = 0U; i < 3U; ++i) {
            const auto value = light_color::effective_channel(settings.channels[i], dimmer);
            static constexpr char digits[] = "0123456789ABCDEF";
            snapshot.rgb[i * 2U] = digits[value >> 4U];
            snapshot.rgb[i * 2U + 1U] = digits[value & 0x0FU];
        }
        snapshot.rgb[6] = '\0';
    }
    return LightApiError::none;
}

LightApiError light_api_set(LightService& light, const std::uint8_t endpoint,
    const LightApiMutation& mutation, LightApiSnapshot& snapshot, PowerOperationSource source) {
    const auto validation = validate(light, endpoint, mutation);
    if (validation != LightApiError::none)
        return validation;

    if (mutation.set_power) {
        const auto result = light.set(static_cast<std::uint8_t>(endpoint + 1U),
            mutation.power ? PowerAction::on : PowerAction::off, source);
        if (result == PowerChangeResult::unavailable)
            return LightApiError::unavailable;
        if (result == PowerChangeResult::failed)
            return LightApiError::hardware_failure;
    }

    const bool has_value_update = mutation.set_brightness || mutation.set_hue ||
                                  mutation.set_saturation || mutation.set_color_temperature ||
                                  mutation.set_rgb || mutation.set_channels;
    if (has_value_update) {
        auto next = light.intent();
        const auto selected = endpoint_channels(light, endpoint);
        const auto previous_brightness = dimmer_for_endpoint(light, endpoint, next);
        const bool channel_change = mutation.set_channels;

        // Match the stable Tasmota setter order: CT, H/S, RGB, channels, bri.
        if (mutation.set_color_temperature) {
            light_color::set_color_temperature(
                light.channel_count(), light.separate(), next, mutation.color_temperature);
            if (!light.separate() || endpoint == 0U)
                next.scheme = 0U;
        }
        if (mutation.set_hue || mutation.set_saturation) {
            std::uint16_t hue = 0U;
            std::uint8_t saturation = 0U;
            light_color::rgb_to_hs(next.channels, hue, saturation);
            if (mutation.set_hue)
                hue = mutation.hue == 360U ? 0U : mutation.hue;
            if (mutation.set_saturation)
                saturation = mutation.saturation;
            std::array<std::uint8_t, 3> rgb{};
            light_color::hsb_to_rgb(hue, saturation, rgb);
            (void)normalize_rgb(rgb, next, !light.separate());
            set_dimmer_for_endpoint(light, endpoint, next, previous_brightness);
            next.scheme = 0U;
        }
        if (mutation.set_rgb) {
            const auto brightness = normalize_rgb(mutation.rgb, next, !light.separate());
            set_dimmer_for_endpoint(light, endpoint, next, brightness);
            next.scheme = 0U;
        }
        if (channel_change) {
            if (light.independent()) {
                materialize_independent_channels(light, next);
                next.channels[selected.first] = mutation.channels[0];
                next.dimmer = 100U;
            } else {
                for (std::size_t i = 0U; i < selected.count; ++i)
                    next.channels[static_cast<std::size_t>(selected.first) + i] =
                        mutation.channels[i];
                const auto dimmer =
                    light_color::normalize_channels(next.channels, selected.first, selected.count);
                set_dimmer_for_endpoint(light, endpoint, next, dimmer);
            }
            const bool white_changed = !light.separate() || endpoint == 1U;
            if (white_changed && (light.channel_count() == 2U || light.channel_count() == 5U))
                light_color::derive_color_temperature(light.channel_count(), next);
            if (!light.separate() || endpoint == 0U)
                next.scheme = 0U;
        }
        if (mutation.set_brightness) {
            if (light.independent()) {
                materialize_independent_channels(light, next);
                next.channels[selected.first] = mutation.brightness;
            } else {
                set_dimmer_for_endpoint(
                    light, endpoint, next, light_color::brightness_percent(mutation.brightness));
            }
        }

        const auto applied = light.update(next, false, selected_channel(light, endpoint));
        if (applied != platform::PwmResult::success)
            return LightApiError::hardware_failure;
        const auto endpoint_bit = static_cast<std::uint8_t>(1U << endpoint);
        if (mutation.set_brightness && mutation.brightness == 0U &&
            (light.intent().desired_power & endpoint_bit) != 0U) {
            const auto power_result =
                light.set(static_cast<std::uint8_t>(endpoint + 1U), PowerAction::off, source);
            if (power_result == PowerChangeResult::failed)
                return LightApiError::hardware_failure;
            if (power_result == PowerChangeResult::unavailable)
                return LightApiError::unavailable;
        }
    }

    return light_api_get(light, endpoint, snapshot);
}

} // namespace newmota
