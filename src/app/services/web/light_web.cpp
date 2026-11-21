/*
  light_web.cpp - browser view of configured Light endpoints

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file light_web.cpp
 * @brief Maps LightService endpoints to the Home light-control contract.
 */

#include "services/web/light_web.h"

#include "core/command/command.h"
#include "services/light/light_api.h"
#include "services/light/light_service.h"
#include "support/number.h"

#include <array>
#include <cstdint>

namespace newmota::light_web {
namespace {

/** @brief Classifies one endpoint by the controls it needs, not by its backend. */
const char* kind(const LightApiSnapshot& snapshot) {
    if (snapshot.has_rgb) {
        return snapshot.channel_count >= 5U   ? "rgbct"
               : snapshot.channel_count == 4U ? "rgbw"
                                              : "rgb";
    }
    return snapshot.has_color_temperature ? "ct" : "dimmer";
}

bool write_endpoint(CommandOutput& output, const LightService& light, const std::uint8_t endpoint,
    const LightApiSnapshot& snapshot) {
    const auto power_index =
        light.endpoints().index(PowerOwner::light, static_cast<std::uint8_t>(endpoint + 1U));
    if (!output.format(
            "{\"Endpoint\":%u,\"Power\":%u,\"Kind\":\"%s\",\"On\":%s,\"Bri\":%u,"
            "\"Hue\":%u,\"Sat\":%u,\"CT\":%u,\"RGB\":\"%s\",\"Scheme\":%u,\"Channels\":[",
            static_cast<unsigned>(endpoint), static_cast<unsigned>(power_index), kind(snapshot),
            snapshot.power ? "true" : "false", static_cast<unsigned>(snapshot.brightness),
            static_cast<unsigned>(snapshot.hue), static_cast<unsigned>(snapshot.saturation),
            static_cast<unsigned>(snapshot.color_temperature), snapshot.rgb.data(),
            static_cast<unsigned>(light.intent().scheme))) {
        return false;
    }
    for (std::size_t i = 0U; i < snapshot.channel_count; ++i) {
        if (!output.format("%s%u", i == 0U ? "" : ",", static_cast<unsigned>(snapshot.channels[i])))
            return false;
    }
    if (!output.write("]"))
        return false;
    // Only a light that switches between white and colour offers the mode button.
    if (light.mode_switch() &&
        !output.format(",\"Mode\":\"%s\"", light.shows_color() ? "color" : "white"))
        return false;
    return output.write("}");
}

int hex_value(const char value) {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    return -1;
}

bool parse_byte(const std::string_view text, std::uint8_t& value) {
    std::uint32_t number = 0U;
    if (!support::parse_unsigned_decimal(text, 255U, number))
        return false;
    value = static_cast<std::uint8_t>(number);
    return true;
}

bool parse_rgb(const std::string_view text, std::array<std::uint8_t, 3>& rgb) {
    if (text.size() != 6U)
        return false;
    for (std::size_t i = 0U; i < 3U; ++i) {
        const auto high = hex_value(text[i * 2U]);
        const auto low = hex_value(text[i * 2U + 1U]);
        if (high < 0 || low < 0)
            return false;
        rgb[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

bool parse_channels(std::string_view text, LightApiMutation& mutation) {
    mutation.channel_count = 0U;
    while (true) {
        const auto comma = text.find(',');
        if (mutation.channel_count == mutation.channels.size() ||
            !parse_byte(text.substr(0U, comma), mutation.channels[mutation.channel_count]))
            return false;
        ++mutation.channel_count;
        if (comma == std::string_view::npos)
            return true;
        text.remove_prefix(comma + 1U);
    }
}

/** @brief Parses the bounded form; each key may appear once and unknown keys are rejected. */
bool parse_form(std::string_view body, std::uint8_t& endpoint, LightApiMutation& mutation) {
    static constexpr std::array<std::string_view, 8> keys{
        "endpoint", "power", "bri", "hue", "sat", "ct", "rgb", "channels"};
    unsigned seen = 0U;
    while (!body.empty()) {
        const auto amp = body.find('&');
        const auto field = body.substr(0U, amp);
        body = amp == std::string_view::npos ? std::string_view{} : body.substr(amp + 1U);
        const auto equal = field.find('=');
        if (equal == std::string_view::npos)
            return false;
        const auto key = field.substr(0U, equal);
        const auto value = field.substr(equal + 1U);
        std::size_t index = 0U;
        while (index < keys.size() && keys[index] != key)
            ++index;
        if (index == keys.size() || (seen & (1U << index)) != 0U)
            return false;
        seen |= 1U << index;
        std::uint32_t number = 0U;
        bool valid = true;
        switch (index) {
        case 0:
            valid = support::parse_unsigned_decimal(value, 31U, number);
            endpoint = static_cast<std::uint8_t>(number);
            break;
        case 1:
            valid = value == "0" || value == "1";
            mutation.set_power = true;
            mutation.power = value == "1";
            break;
        case 2:
            valid = parse_byte(value, mutation.brightness);
            mutation.set_brightness = true;
            break;
        case 3:
            valid = support::parse_unsigned_decimal(value, 360U, number);
            mutation.hue = static_cast<std::uint16_t>(number);
            mutation.set_hue = true;
            break;
        case 4:
            valid = parse_byte(value, mutation.saturation);
            mutation.set_saturation = true;
            break;
        case 5:
            valid = support::parse_unsigned_decimal(value, 500U, number);
            mutation.color_temperature = static_cast<std::uint16_t>(number);
            mutation.set_color_temperature = true;
            break;
        case 6:
            valid = parse_rgb(value, mutation.rgb);
            mutation.set_rgb = true;
            break;
        case 7:
            valid = parse_channels(value, mutation);
            mutation.set_channels = true;
            break;
        default:
            valid = false;
        }
        if (!valid)
            return false;
    }
    return (seen & 1U) != 0U;
}

} // namespace

bool write_lights(CommandOutput& output, const LightService* const light) {
    if (!output.write("{\"Lights\":["))
        return false;
    const auto count = light != nullptr && light->available() ? light->endpoint_count() : 0U;
    for (std::uint8_t endpoint = 0U; endpoint < count; ++endpoint) {
        LightApiSnapshot snapshot{};
        if (light_api_get(*light, endpoint, snapshot) != LightApiError::none)
            continue;
        if ((endpoint != 0U && !output.write(",")) ||
            !write_endpoint(output, *light, endpoint, snapshot))
            return false;
    }
    return output.write("]}");
}

Result apply(LightService* const light, const std::string_view body, CommandOutput& output) {
    std::uint8_t endpoint = 0U;
    LightApiMutation mutation{};
    if (!parse_form(body, endpoint, mutation))
        return Result::invalid;
    if (light == nullptr)
        return Result::unavailable;
    // Like Tasmota's Dimmer and Color commands, a visible value change switches the light on;
    // brightness zero switches it off inside the Light API.
    const bool value_change = (mutation.set_brightness && mutation.brightness != 0U) ||
                              mutation.set_hue || mutation.set_saturation || mutation.set_rgb ||
                              mutation.set_color_temperature || mutation.set_channels;
    if (value_change && !mutation.set_power) {
        mutation.set_power = true;
        mutation.power = true;
    }
    LightApiSnapshot snapshot{};
    switch (light_api_set(*light, endpoint, mutation, snapshot, PowerOperationSource::web_gui)) {
    case LightApiError::none:
        break;
    case LightApiError::invalid_endpoint:
    case LightApiError::invalid_request:
        return Result::invalid;
    case LightApiError::unavailable:
        return Result::unavailable;
    case LightApiError::hardware_failure:
        return Result::failed;
    }
    return write_endpoint(output, *light, endpoint, snapshot) ? Result::ok : Result::failed;
}

} // namespace newmota::light_web
