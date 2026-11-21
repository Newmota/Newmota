/*
  light_commands.cpp - portable PWM and Light behavior for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file light_commands.cpp
 * @brief Portable PWM and Light behavior for Newmota.
 */

#include "services/light/light_commands.h"
#include "services/light/light_color.h"
#include "services/light/light_service.h"
#include "newmota/log.h"
#include "support/number.h"
#include "support/text.h"
#include <algorithm>
#include <array>
#include <memory>
#include <new>

namespace newmota::commands {
namespace {
CommandStatus result(const platform::PwmResult status, CommandOutput& output) {
    if (status == platform::PwmResult::success)
        return CommandStatus::handled;
    const char* reason = "NativeFailure";
    switch (status) {
    case platform::PwmResult::ownership_conflict:
        reason = "OwnershipConflict";
        break;
    case platform::PwmResult::invalid_request:
        reason = "InvalidRequest";
        break;
    case platform::PwmResult::unsupported_route:
        reason = "UnsupportedRoute";
        break;
    case platform::PwmResult::requirements_unsatisfied:
        reason = "RequirementsUnsatisfied";
        break;
    case platform::PwmResult::resource_unavailable:
        reason = "ResourceUnavailable";
        break;
    case platform::PwmResult::incompatible:
        reason = "Incompatible";
        break;
    case platform::PwmResult::faulted:
        reason = "Faulted";
        break;
    case platform::PwmResult::quarantined:
        reason = "Quarantined";
        break;
    case platform::PwmResult::no_memory:
        reason = "OutOfMemory";
        break;
    default:
        break;
    }
    (void)output.format("{\"Error\":\"%s\"}", reason);
    return CommandStatus::failed;
}

bool number(const CommandRequest& request, const std::uint32_t max, std::uint32_t& value) {
    return support::parse_unsigned_decimal(support::trim_ascii(request.payload), max, value);
}

bool scalar_index(const CommandRequest& request) {
    return !request.has_index || request.index == 1U;
}

CommandStatus scalar(CommandOutput& output, const char* name, const std::uint32_t value) {
    return output.format("{\"%s\":%lu}", name, static_cast<unsigned long>(value))
               ? CommandStatus::handled
               : CommandStatus::failed;
}

#if USE_PWM
bool pwm_state(LightService& light, CommandOutput& output) {
    if (!output.write("{\"PWM\":{"))
        return false;
    bool first = true;
    for (std::size_t i = 0U; i < 16U; ++i) {
        if (!light.outputs().configured(i) || light.owns(i))
            continue;
        if (!output.format("%s\"PWM%u\":%u", first ? "" : ",", static_cast<unsigned>(i + 1U),
                static_cast<unsigned>(light.intent().raw[i])))
            return false;
        first = false;
    }
    return output.write("}}");
}
#endif

int hex(const char value) {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

std::uint32_t scale_color(
    const std::uint32_t value, const std::uint32_t source_max, const std::uint32_t target_max) {
    return light_color::scale(value, source_max, target_max);
}

bool parse_hsb(
    std::string_view text, std::array<std::uint16_t, 3>& values, std::size_t& parsed_count) {
    constexpr std::array<std::uint32_t, 3> limits{{360U, 100U, 100U}};
    text = support::trim_ascii(text);
    std::size_t offset = 0U;
    parsed_count = 0U;
    if (text.empty())
        return false;
    while (offset <= text.size()) {
        if (parsed_count == values.size())
            return false;
        const auto separator = text.find(',', offset);
        const auto end = separator == std::string_view::npos ? text.size() : separator;
        const auto component = support::trim_ascii(text.substr(offset, end - offset));
        std::uint32_t value = 0U;
        if (!support::parse_unsigned_decimal(component, limits[parsed_count], value))
            return false;
        values[parsed_count++] = static_cast<std::uint16_t>(value);
        if (separator == std::string_view::npos)
            break;
        offset = separator + 1U;
    }
    return parsed_count != 0U;
}

void hsb_to_rgb(const std::uint16_t input_hue, const std::uint8_t saturation,
    std::array<std::uint8_t, 3>& channels) {
    light_color::hsb_to_rgb(input_hue, saturation, channels);
}

void rgb_to_hs(
    const std::array<std::uint8_t, 5>& channels, std::uint16_t& hue, std::uint8_t& saturation) {
    light_color::rgb_to_hs(channels, hue, saturation);
}

bool fixed_color(
    const std::uint32_t preset, const std::size_t count, std::array<std::uint8_t, 5>& channels) {
    constexpr std::array<std::array<std::uint8_t, 3>, 12> rgb{{
        {{255U, 0U, 0U}},
        {{0U, 255U, 0U}},
        {{0U, 0U, 255U}},
        {{228U, 32U, 0U}},
        {{0U, 228U, 32U}},
        {{0U, 32U, 228U}},
        {{188U, 64U, 0U}},
        {{0U, 160U, 96U}},
        {{160U, 32U, 240U}},
        {{255U, 255U, 0U}},
        {{255U, 0U, 170U}},
        {{255U, 255U, 255U}},
    }};
    constexpr std::array<std::uint8_t, 4> white{{0U, 255U, 128U, 32U}};
    constexpr std::array<std::array<std::uint8_t, 2>, 4> cold_warm{{
        {{0U, 0U}},
        {{255U, 0U}},
        {{0U, 255U}},
        {{128U, 128U}},
    }};

    if (preset >= 1U && preset <= rgb.size() && count >= 3U) {
        channels.fill(0U);
        for (std::size_t i = 0U; i < rgb[preset - 1U].size(); ++i)
            channels[i] = rgb[preset - 1U][i];
        return true;
    }
    if (preset < 200U || preset > 203U)
        return false;

    channels.fill(0U);
    const auto index = static_cast<std::size_t>(preset - 200U);
    if (count == 2U) {
        channels[0] = cold_warm[index][0];
        channels[1] = cold_warm[index][1];
        return true;
    }
    if (count == 4U) {
        channels[3] = white[index];
        return true;
    }
    if (count == 5U) {
        channels[3] = cold_warm[index][0];
        channels[4] = cold_warm[index][1];
        return true;
    }
    return false;
}

bool parse_color(
    std::string_view text, const std::size_t count, std::array<std::uint8_t, 5>& channels) {
    text = support::trim_ascii(text);
    if (!text.empty() && text.front() == '#')
        text.remove_prefix(1U);
    if (text.find(',') != std::string_view::npos) {
        std::size_t offset = 0U;
        for (std::size_t i = 0U; i < count; ++i) {
            const auto separator = text.find(',', offset);
            const bool last = i + 1U == count;
            if ((last && separator != std::string_view::npos) ||
                (!last && separator == std::string_view::npos))
                return false;
            const auto end = last ? text.size() : separator;
            const auto component = support::trim_ascii(text.substr(offset, end - offset));
            std::uint32_t value = 0U;
            if (!support::parse_unsigned_decimal(component, 255U, value))
                return false;
            channels[i] = static_cast<std::uint8_t>(value);
            offset = last ? text.size() : separator + 1U;
        }
        return offset == text.size();
    }
    if (text.size() == count * 2U) {
        for (std::size_t i = 0U; i < count; ++i) {
            const int high = hex(text[i * 2U]), low = hex(text[i * 2U + 1U]);
            if (high < 0 || low < 0)
                return false;
            channels[i] = static_cast<std::uint8_t>(high * 16 + low);
        }
        return true;
    }
    if (text.size() > 3U)
        return false;
    std::uint32_t preset = 0U;
    if (!support::parse_unsigned_decimal(text, 203U, preset))
        return false;
    return fixed_color(preset, count, channels);
}

void derive_ct(const LightService& light, LightSettings& next) {
    light_color::derive_color_temperature(light.channel_count(), next);
}

void select_white(const LightService& light, LightSettings& next, const std::uint8_t brightness) {
    light_color::select_white(light.channel_count(), next, brightness);
}

std::uint32_t white_brightness(const LightService& light) {
    unsigned total = 0U;
    if (light.channel_count() == 4U)
        total = light.intent().channels[3];
    else {
        const auto offset = light.channel_count() == 2U ? 0U : 3U;
        total = static_cast<unsigned>(light.intent().channels[offset]) +
                light.intent().channels[offset + 1U];
    }
    const auto effective =
        (total * (light.separate() ? light.intent().white_dimmer : light.intent().dimmer) + 50U) /
        100U;
    return scale_color(effective, 255U, 100U);
}

CommandStatus white_state(const LightService& light, CommandOutput& output) {
    return scalar(output, "White", white_brightness(light));
}

CommandStatus color_components(CommandOutput& output, const char* name,
    const std::uint8_t* channels, const std::size_t count, const bool decimal) {
    if (!output.format("{\"%s\":\"", name))
        return CommandStatus::failed;
    for (std::size_t i = 0U; i < count; ++i) {
        if (decimal) {
            if (i != 0U && !output.write(","))
                return CommandStatus::failed;
            if (!output.format("%u", static_cast<unsigned>(channels[i])))
                return CommandStatus::failed;
        } else if (!output.format("%02X", static_cast<unsigned>(channels[i])))
            return CommandStatus::failed;
    }
    return output.write("\"}") ? CommandStatus::handled : CommandStatus::failed;
}

CommandStatus color_state(const LightService& light, CommandOutput& output) {
    std::array<std::uint8_t, 5> channels{};
    for (std::size_t i = 0U; i < light.channel_count(); ++i) {
        channels[i] = static_cast<std::uint8_t>(
            (static_cast<unsigned>(light.intent().channels[i]) *
                    (light.separate() && i >= 3U ? light.intent().white_dimmer
                                                 : light.intent().dimmer) +
                50U) /
            100U);
    }
    return color_components(
        output, "Color", channels.data(), light.channel_count(), light.option(17U) != 0U);
}

CommandStatus hsb_state(const LightService& light, CommandOutput& output) {
    std::uint16_t hue = 0U;
    std::uint8_t saturation = 0U;
    rgb_to_hs(light.intent().channels, hue, saturation);
    const auto rgb_peak = std::max(
        {light.intent().channels[0], light.intent().channels[1], light.intent().channels[2]});
    const auto rgb_brightness =
        (static_cast<std::uint32_t>(rgb_peak) * light.intent().dimmer + 50U) / 100U;
    return output.format("{\"HsbColor\":\"%u,%u,%u\"}", static_cast<unsigned>(hue),
               static_cast<unsigned>(scale_color(saturation, 255U, 100U)),
               static_cast<unsigned>(scale_color(rgb_brightness, 255U, 100U)))
               ? CommandStatus::handled
               : CommandStatus::failed;
}

CommandStatus hsb_component_state(
    const LightService& light, const std::uint16_t component, CommandOutput& output) {
    std::uint16_t hue = 0U;
    std::uint8_t saturation = 0U;
    rgb_to_hs(light.intent().channels, hue, saturation);
    const auto rgb_peak = std::max(
        {light.intent().channels[0], light.intent().channels[1], light.intent().channels[2]});
    const auto rgb_brightness =
        (static_cast<std::uint32_t>(rgb_peak) * light.intent().dimmer + 50U) / 100U;
    constexpr std::array<const char*, 3> names{{"HsbColor1", "HsbColor2", "HsbColor3"}};
    const std::array<std::uint32_t, 3> values{
        {hue, scale_color(saturation, 255U, 100U), scale_color(rgb_brightness, 255U, 100U)}};
    return scalar(output, names[component - 1U], values[component - 1U]);
}

bool percent(const CommandRequest& request, const std::uint32_t old, std::uint32_t& value) {
    if (request.payload == "+") {
        value = std::min<std::uint32_t>(100U, old + 10U);
        return true;
    }
    if (request.payload == "-") {
        value = old > 10U ? old - 10U : 1U;
        return true;
    }
    return number(request, 100U, value);
}

#if USE_WS2812
/** @brief Consumes one blank-separated LED color, preserving the command's token rules. */
std::string_view next_pixel_color(std::string_view& text) {
    const auto end = text.find(' ');
    const auto token = support::trim_ascii(text.substr(0U, end));
    text = end == std::string_view::npos ? std::string_view{}
                                         : support::trim_ascii(text.substr(end + 1U));
    return token;
}

bool parse_pixel_color(
    const std::string_view text, const std::uint8_t count, led_strip::Pixel& pixel) {
    std::array<std::uint8_t, 5> channels{};
    if (!parse_color(text, count, channels))
        return false;
    pixel = {channels[0], channels[1], channels[2],
        static_cast<std::uint8_t>(count == 4U ? channels[3] : 0U)};
    return true;
}

CommandStatus pixel_state(
    const LightService& light, const std::uint16_t index, CommandOutput& output) {
    led_strip::Pixel pixel{};
    const auto status = light.pixel(static_cast<std::size_t>(index - 1U), pixel);
    if (status != platform::PwmResult::success)
        return result(status, output);
    constexpr char name[] = "Led";
    if (!output.format("{\"%s%u\":\"", name, static_cast<unsigned>(index)))
        return CommandStatus::failed;
    const std::array<std::uint8_t, 4> channels{{pixel.r, pixel.g, pixel.b, pixel.w}};
    const auto count = light.channel_count();
    const bool decimal = light.option(17U) != 0U;
    for (std::size_t i = 0U; i < count; ++i) {
        if (decimal) {
            if (i != 0U && !output.write(","))
                return CommandStatus::failed;
            if (!output.format("%u", static_cast<unsigned>(channels[i])))
                return CommandStatus::failed;
        } else if (!output.format("%02X", static_cast<unsigned>(channels[i])))
            return CommandStatus::failed;
    }
    return output.write("\"}") ? CommandStatus::handled : CommandStatus::failed;
}

CommandStatus clock_color_state(
    const LightService& light, const std::uint8_t color, CommandOutput& output) {
    const auto& channels = light.intent().strip_clock_colors[color - 3U];
    constexpr std::array<const char*, 4> names{{"Color3", "Color4", "Color5", "Color6"}};
    const char* const name = names[color - 3U];
    return color_components(output, name, channels.data(), 3U, light.option(17U) != 0U);
}

bool parse_pixels_payload(std::string_view payload, LightSettings& next) {
    std::array<std::uint32_t, 4> values{{next.strip_pixels, 0U, 1U, 0U}};
    std::size_t offset = 0U;
    std::size_t count = 0U;
    while (offset <= payload.size()) {
        if (count == values.size())
            return false;
        const auto separator = payload.find(',', offset);
        const auto end = separator == std::string_view::npos ? payload.size() : separator;
        if (!support::parse_unsigned_decimal(
                support::trim_ascii(payload.substr(offset, end - offset)), UINT32_MAX,
                values[count]))
            return false;
        ++count;
        if (separator == std::string_view::npos)
            break;
        offset = separator + 1U;
    }
    if (count == 0U || values[0] == 0U || values[0] > platform::led_strip::kMaximumPixels ||
        values[2] > 32768U)
        return false;
    next.strip_pixels = static_cast<std::uint16_t>(values[0]);
    next.strip_reverse = (values[1] & 1U) != 0U;
    next.strip_height = static_cast<std::uint16_t>(values[2] == 0U ? 1U : values[2]);
    next.strip_alternate = (values[3] & 1U) != 0U;
    if (next.strip_rotation >= next.strip_pixels)
        next.strip_rotation = 0U;
    return true;
}
#endif
} // namespace

CommandStatus dispatch_light(
    const CommandRequest& request, LightService& light, CommandOutput& output) {
    auto next = light.intent();
#if USE_PWM
    auto effective_request = request;
    if (support::ascii_iequals(request.name, "pwmfrequency") && request.has_index)
        return CommandStatus::invalid;
    if (support::ascii_iequals(request.name, "pwmfrequency")) {
        const auto separator = request.payload.find(',');
        if (separator != std::string_view::npos) {
            if (request.has_index)
                return CommandStatus::invalid;
            std::uint32_t channel_index = 0U;
            if (!support::parse_unsigned_decimal(
                    support::trim_ascii(request.payload.substr(separator + 1U)), 16U,
                    channel_index) ||
                channel_index == 0U)
                return CommandStatus::invalid;
            effective_request.payload = support::trim_ascii(request.payload.substr(0U, separator));
            effective_request.has_index = true;
            effective_request.index = static_cast<std::uint16_t>(channel_index);
        }
    }
    const auto& timing_request = effective_request;
#endif
#if USE_WS2812
    if (support::ascii_iequals(request.name, "led")) {
        if (!light.addressable() || !request.has_index || request.index == 0U ||
            request.index > light.pixel_count())
            return CommandStatus::invalid;
        const auto pixel_index = static_cast<std::size_t>(request.index - 1U);
        if (!request.is_query()) {
            auto text = support::trim_ascii(request.payload);
            std::size_t count = 0U;
            const auto remaining = light.pixel_count() - pixel_index;
            // Colors beyond the end of the strip retain the existing truncation behavior.
            while (!text.empty() && count < remaining) {
                (void)next_pixel_color(text);
                ++count;
            }
            if (count == 0U)
                return CommandStatus::invalid;
            const auto pixels =
                std::unique_ptr<led_strip::Pixel[]>{new (std::nothrow) led_strip::Pixel[count]};
            if (!pixels) {
                AddLog(LogLevel::error, "LGT: LED command workspace allocation failed");
                return result(platform::PwmResult::resource_unavailable, output);
            }
            text = support::trim_ascii(request.payload);
            for (std::size_t index = 0U; index < count; ++index)
                if (!parse_pixel_color(
                        next_pixel_color(text), light.channel_count(), pixels[index]))
                    return CommandStatus::invalid;
            const auto applied = light.pixels(pixel_index, pixels.get(), count);
            if (applied != platform::PwmResult::success)
                return result(applied, output);
        }
        return pixel_state(light, request.index, output);
    }
    if (support::ascii_iequals(request.name, "pixels")) {
        if (!light.addressable() || request.has_index)
            return CommandStatus::invalid;
        if (!request.is_query()) {
            auto next = light.intent();
            if (!parse_pixels_payload(request.payload, next))
                return CommandStatus::invalid;
            const auto applied = light.update(next, false);
            if (applied != platform::PwmResult::success)
                return result(applied, output);
        }
        const auto& settings = light.intent();
        return output.format("{\"Pixels\":%u,\"PixelsReverse\":%u,"
                             "\"PixelsHeight\":%u,\"PixelsAlternate\":%u}",
                   static_cast<unsigned>(settings.strip_pixels), settings.strip_reverse ? 1U : 0U,
                   static_cast<unsigned>(settings.strip_height), settings.strip_alternate ? 1U : 0U)
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    }
    if (support::ascii_iequals(request.name, "pixeltype")) {
        if (!light.addressable() || !scalar_index(request))
            return CommandStatus::invalid;
        if (!request.is_query()) {
            std::uint32_t value = 0U;
            if (!number(request, 31U, value))
                return CommandStatus::invalid;
            const auto applied = light.pixel_type(static_cast<std::uint8_t>(value));
            if (applied != platform::PwmResult::success)
                return result(applied, output);
        }
        return scalar(output, "PixelType", light.intent().strip_type);
    }
    if (support::ascii_iequals(request.name, "rotation")) {
        if (!light.addressable() || !scalar_index(request))
            return CommandStatus::invalid;
        if (!request.is_query()) {
            std::uint32_t value = 0U;
            if (!number(request, static_cast<std::uint32_t>(light.pixel_count() - 1U), value))
                return CommandStatus::invalid;
            auto next = light.intent();
            next.strip_rotation = static_cast<std::uint16_t>(value);
            const auto applied = light.update(next, false);
            if (applied != platform::PwmResult::success)
                return result(applied, output);
        }
        return scalar(output, "Rotation", light.intent().strip_rotation);
    }
    if (support::ascii_iequals(request.name, "steppixels")) {
        if (!light.addressable() || !scalar_index(request))
            return CommandStatus::invalid;
        if (!request.is_query()) {
            std::uint32_t value = 0U;
            if (!number(request, 255U, value))
                return CommandStatus::invalid;
            auto next = light.intent();
            next.strip_step_pixels = static_cast<std::uint16_t>(value);
            const auto applied = light.update(next, false);
            if (applied != platform::PwmResult::success)
                return result(applied, output);
        }
        return scalar(output, "StepPixels", light.intent().strip_step_pixels);
    }
    if (support::ascii_iequals(request.name, "width")) {
        const auto index = request.has_index ? request.index : 1U;
        if (!light.addressable() || index == 0U || index > 4U)
            return CommandStatus::invalid;
        if (!request.is_query()) {
            std::uint32_t value = 0U;
            const auto maximum = index == 1U ? 4U : 31U;
            if (!number(request, maximum, value))
                return CommandStatus::invalid;
            auto next = light.intent();
            next.strip_width[index - 1U] = static_cast<std::uint8_t>(value);
            const auto applied = light.update(next, false);
            if (applied != platform::PwmResult::success)
                return result(applied, output);
        }
        constexpr std::array<const char*, 4> names{{"Width", "Width2", "Width3", "Width4"}};
        return scalar(output, names[index - 1U], light.intent().strip_width[index - 1U]);
    }
    if (support::ascii_iequals(request.name, "color") && request.has_index && request.index >= 3U &&
        request.index <= 6U) {
        if (!light.addressable())
            return CommandStatus::invalid;
        if (!request.is_query()) {
            std::array<std::uint8_t, 5> channels{};
            if (!parse_color(request.payload, 3U, channels))
                return CommandStatus::invalid;
            auto next = light.intent();
            auto& color = next.strip_clock_colors[request.index - 3U];
            color = {};
            std::copy_n(channels.begin(), 3U, color.begin());
            const auto applied = light.update(next, false);
            if (applied != platform::PwmResult::success)
                return result(applied, output);
        }
        return clock_color_state(light, static_cast<std::uint8_t>(request.index), output);
    }
#endif
#if USE_PWM
    if (support::ascii_iequals(request.name, "pwm")) {
        const auto index = request.has_index ? request.index : 1U;
        if (index == 0U || index > 16U || !light.outputs().configured(index - 1U))
            return CommandStatus::invalid;
        if (light.owns(index - 1U))
            return result(platform::PwmResult::ownership_conflict, output);
        if (!request.is_query()) {
            std::uint32_t duty = 0U;
            if (!number(request, next.range, duty))
                return CommandStatus::invalid;
            const auto applied = light.raw_write(index - 1U, static_cast<std::uint16_t>(duty));
            if (applied != platform::PwmResult::success)
                return result(applied, output);
        }
        return pwm_state(light, output) ? CommandStatus::handled : CommandStatus::failed;
    }
    if (support::ascii_iequals(timing_request.name, "pwmrange") ||
        support::ascii_iequals(timing_request.name, "pwmfrequency")) {
        const bool indexed_frequency =
            support::ascii_iequals(timing_request.name, "pwmfrequency") && timing_request.has_index;
        if ((!indexed_frequency && !scalar_index(timing_request)) ||
            (indexed_frequency && (timing_request.index == 0U || timing_request.index > 16U ||
                                      !light.outputs().configured(timing_request.index - 1U))))
            return CommandStatus::invalid;
        const bool range = support::ascii_iequals(timing_request.name, "pwmrange");
        if (!timing_request.is_query()) {
            std::uint32_t value = 0U;
            if (!number(timing_request, range ? 1023U : 50000U, value))
                return CommandStatus::invalid;
            if (range) {
                if (value != 1U && value < 255U)
                    return CommandStatus::invalid;
                next.range = value == 1U     ? 1023U
                             : value == 255U ? 255U
                             : value <= 511U ? 511U
                                             : 1023U;
                for (auto& duty : next.raw)
                    duty = std::min(duty, next.range);
            } else {
                if (value == 0U)
                    return CommandStatus::invalid;
                if (indexed_frequency)
                    next.channel_frequency[timing_request.index - 1U] = value == 1U ? 0U : value;
                else {
                    next.frequency = value == 1U ? 977U : value;
                    next.channel_frequency.fill(0U);
                }
            }
            const auto applied = light.timing(next,
                indexed_frequency ? static_cast<std::uint16_t>(1U << (timing_request.index - 1U))
                                  : light.outputs().mask());
            if (applied != platform::PwmResult::success)
                return result(applied, output);
        }
        if (indexed_frequency) {
            return output.format("{\"PWMFrequency\":%lu%s}",
                       static_cast<unsigned long>(
                           light.intent().carrier(timing_request.index - 1U)),
                       light.timing_pending() ? ",\"RestartRequired\":true" : "")
                       ? CommandStatus::handled
                       : CommandStatus::failed;
        }
        if (light.timing_pending()) {
            return output.format("{\"%s\":%lu,\"RestartRequired\":true}",
                       range ? "PWMRange" : "PWMFrequency",
                       static_cast<unsigned long>(
                           range ? light.intent().range : light.intent().frequency))
                       ? CommandStatus::handled
                       : CommandStatus::failed;
        }
        return scalar(output, range ? "PWMRange" : "PWMFrequency",
            range ? light.intent().range : light.intent().frequency);
    }
#endif
    if (support::ascii_iequals(request.name, "palette")) {
        if (!scalar_index(request) || !light.available() ||
            (light.independent() && !light.addressable()) || light.channel_count() < 3U)
            return CommandStatus::invalid;
        if (!request.is_query()) {
            next.palette = {};
            next.palette_count = next.palette_index = 0U;
            auto text = support::trim_ascii(request.payload);
            if (text != "0") {
                while (!text.empty()) {
                    if (next.palette_count == next.palette.size())
                        return CommandStatus::invalid;
                    const auto end = text.find(' ');
                    const auto token = text.substr(0U, end);
                    auto& entry = next.palette[next.palette_count];
                    if (!parse_color(token, light.channel_count(), entry))
                        return CommandStatus::invalid;
                    ++next.palette_count;
                    if (end == std::string_view::npos)
                        break;
                    text = support::trim_ascii(text.substr(end + 1U));
                }
                if (next.palette_count == 0U)
                    return CommandStatus::invalid;
            }
            const auto applied = light.update(next, false);
            if (applied != platform::PwmResult::success)
                return result(applied, output);
        }
        if (!output.write("{\"Palette\":["))
            return CommandStatus::failed;
        for (std::size_t i = 0U; i < light.intent().palette_count; ++i) {
            if (!output.write(i == 0U ? "\"" : ",\""))
                return CommandStatus::failed;
            for (std::size_t j = 0U; j < light.channel_count(); ++j)
                if (!output.format("%02X", light.intent().palette[i][j]))
                    return CommandStatus::failed;
            if (!output.write("\""))
                return CommandStatus::failed;
        }
        return output.write("]}") ? CommandStatus::handled : CommandStatus::failed;
    }
    if (support::ascii_iequals(request.name, "scheme")) {
        if (!scalar_index(request) || !light.available() ||
            (light.independent() && !light.addressable()) || light.channel_count() < 3U)
            return CommandStatus::invalid;
        if (!request.is_query()) {
            const auto maximum = light.addressable() ? 14U : 4U;
            auto scheme_request = request;
            const auto separator = request.payload.find(',');
            std::uint32_t start = 0U;
            const bool has_start = separator != std::string_view::npos;
            if (has_start) {
                scheme_request.payload = support::trim_ascii(request.payload.substr(0U, separator));
                if (!support::parse_unsigned_decimal(
                        support::trim_ascii(request.payload.substr(separator + 1U)), 255U, start))
                    return CommandStatus::invalid;
            }
            std::uint32_t value = next.scheme;
            if (scheme_request.payload == "+") {
                value = next.scheme <= 1U        ? 2U
                        : next.scheme >= maximum ? maximum
                                                 : next.scheme + 1U;
            } else if (scheme_request.payload == "-") {
                value = next.scheme <= 2U ? 0U : next.scheme - 1U;
            } else if (!number(scheme_request, maximum, value))
                return CommandStatus::invalid;
            if (has_start && value < 2U)
                return CommandStatus::invalid;
            if (has_start && next.palette_count != 0U) {
                if (start == 0U || start > next.palette_count)
                    return CommandStatus::invalid;
                --start;
            }
            next.scheme = static_cast<std::uint8_t>(value);
            const auto applied = light.update(next, value != 0U, light.separate() ? 1U : 0U,
                has_start ? static_cast<std::uint16_t>(start) : 256U);
            if (applied != platform::PwmResult::success)
                return result(applied, output);
        }
        return scalar(output, "Scheme", light.intent().scheme);
    }
    if (support::ascii_iequals(request.name, "wakeup")) {
        if (!scalar_index(request) || !light.available() ||
            (light.independent() && !light.addressable()) || light.channel_count() < 3U)
            return CommandStatus::invalid;
        if (request.is_query()) {
            return output.format("{\"Wakeup\":\"%s\"}",
                       next.scheme == 1U && (light.addressable() ? next.desired_power != 0U
                                                                 : (next.desired_power & 1U) != 0U)
                           ? "Running"
                           : "Idle")
                       ? CommandStatus::handled
                       : CommandStatus::failed;
        }
        if (!request.is_query()) {
            std::uint32_t value = 0U;
            if (!number(request, 100U, value))
                return CommandStatus::invalid;
            next.dimmer = static_cast<std::uint8_t>(value);
        }
        next.scheme = 1U;
        const auto applied = light.update(next, true, light.separate() ? 1U : 0U);
        if (applied != platform::PwmResult::success)
            return result(applied, output);
        return output.format(
                   "{\"Wakeup\":\"%s\"}", light.intent().scheme == 1U ? "Started" : "Idle")
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    }
    if (support::ascii_iequals(request.name, "rgbwwtable")) {
        if (!scalar_index(request) || !light.available())
            return CommandStatus::invalid;
        if (!request.is_query()) {
            // Up to five values replace the table from the first channel on.
            auto text = support::trim_ascii(request.payload);
            for (std::size_t i = 0U;; ++i) {
                const auto end = text.find(',');
                std::uint32_t value = 0U;
                if (i == next.rgbww_table.size() ||
                    !support::parse_unsigned_decimal(
                        support::trim_ascii(text.substr(0U, end)), 255U, value))
                    return CommandStatus::invalid;
                next.rgbww_table[i] = static_cast<std::uint8_t>(value);
                if (end == std::string_view::npos)
                    break;
                text = text.substr(end + 1U);
            }
            const auto applied = light.update(next, false);
            if (applied != platform::PwmResult::success)
                return result(applied, output);
        }
        const auto& table = light.intent().rgbww_table;
        return output.format("{\"RGBWWTable\":\"%u,%u,%u,%u,%u\"}", unsigned{table[0]},
                   unsigned{table[1]}, unsigned{table[2]}, unsigned{table[3]}, unsigned{table[4]})
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    }
#if USE_LED_DRIVER
    if (support::ascii_iequals(request.name, "lightcurrent")) {
        const auto* chip = light.led_driver();
        if (!scalar_index(request) || chip == nullptr)
            return CommandStatus::invalid;
        if (!request.is_query()) {
            // RGB current, then white; one value sets both. Units are the chip's.
            const auto text = support::trim_ascii(request.payload);
            const auto comma = text.find(',');
            std::uint32_t rgb = 0U;
            std::uint32_t white = 0U;
            if (!support::parse_unsigned_decimal(
                    support::trim_ascii(text.substr(0U, comma)), 255U, rgb))
                return CommandStatus::invalid;
            white = rgb;
            if (comma != std::string_view::npos &&
                !support::parse_unsigned_decimal(
                    support::trim_ascii(text.substr(comma + 1U)), 255U, white))
                return CommandStatus::invalid;
            if (!chip->valid_current(static_cast<std::uint8_t>(rgb), false) ||
                !chip->valid_current(static_cast<std::uint8_t>(white), true))
                return CommandStatus::invalid;
            next.led_current = {{static_cast<std::uint8_t>(rgb), static_cast<std::uint8_t>(white)}};
            const auto applied = light.update(next, false);
            if (applied != platform::PwmResult::success)
                return result(applied, output);
        }
        const auto current = chip->currents(light.intent().led_current);
        return output.format(
                   "{\"LightCurrent\":\"%u,%u\"}", unsigned{current[0]}, unsigned{current[1]})
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    }
#endif
    if (support::ascii_iequals(request.name, "wakeupduration")) {
        if (!scalar_index(request) || !light.available() ||
            (light.independent() && !light.addressable()) || light.channel_count() < 3U)
            return CommandStatus::invalid;
        if (!request.is_query()) {
            std::uint32_t value = 0U;
            if (!number(request, 3000U, value) || value == 0U)
                return CommandStatus::invalid;
            next.wakeup_duration = static_cast<std::uint16_t>(value);
            if (next.scheme == 1U)
                next.scheme = 0U;
            const auto applied = light.update(next, false);
            if (applied != platform::PwmResult::success)
                return result(applied, output);
        }
        return scalar(output, "WakeupDuration", light.intent().wakeup_duration);
    }
    const bool dimmer = support::ascii_iequals(request.name, "dimmer");
    const bool channel = support::ascii_iequals(request.name, "channel");
    const bool color = support::ascii_iequals(request.name, "color");
    const bool hsb = support::ascii_iequals(request.name, "hsbcolor");
    const bool white = support::ascii_iequals(request.name, "white");
    const bool ct = support::ascii_iequals(request.name, "ct") ||
                    support::ascii_iequals(request.name, "colortemperature");
    const bool fade = support::ascii_iequals(request.name, "fade");
    const bool gamma = support::ascii_iequals(request.name, "ledtable");
    const bool speed = support::ascii_iequals(request.name, "speed");
    if (!dimmer && !channel && !color && !hsb && !white && !ct && !fade && !gamma && !speed)
        return CommandStatus::unknown;
    if (!light.available())
        return CommandStatus::invalid;
    if (white) {
        const auto count = light.channel_count();
        if ((request.has_index && request.index != 1U) || light.independent() ||
            (count != 2U && count != 4U && count != 5U))
            return CommandStatus::invalid;
        if (!request.is_query()) {
            std::uint32_t value = 0U;
            if (!number(request, 100U, value))
                return CommandStatus::invalid;
            const auto previous = next;
            select_white(light, next, static_cast<std::uint8_t>(value));
            if (light.separate()) {
                for (std::size_t i = 0U; i < 3U; ++i)
                    next.channels[i] = previous.channels[i];
                next.dimmer = previous.dimmer;
                next.white_dimmer = static_cast<std::uint8_t>(value);
            }
            if (!light.separate())
                next.scheme = 0U;
            const auto applied = light.update(next, true, light.separate() ? 4U : 0U);
            if (applied != platform::PwmResult::success)
                return result(applied, output);
        }
        return white_state(light, output);
    }
    if (channel) {
        // Tasmota's Channel index starts at the first Light power device index.
        const auto first = light.endpoints().index(PowerOwner::light, 1U);
        const auto index = request.has_index ? request.index : 1U;
        if (index < first || index >= first + light.channel_count())
            return CommandStatus::invalid;
        const auto local = static_cast<std::uint8_t>(index - first);
        if (!request.is_query()) {
            std::uint32_t value = 0U;
            if (!percent(request,
                    (next.channels[local] * static_cast<unsigned>(light.separate() && local >= 3U
                                                                      ? next.white_dimmer
                                                                      : next.dimmer) +
                        127U) /
                        255U,
                    value))
                return CommandStatus::invalid;
            const bool white_group = light.separate() && local >= 3U;
            const auto group_begin = white_group ? 3U : 0U;
            const auto group_end = light.separate() && !white_group ? 3U : light.channel_count();
            const auto group_dimmer = white_group ? next.white_dimmer : next.dimmer;
            for (std::size_t i = group_begin; i < group_end; ++i)
                next.channels[i] = static_cast<std::uint8_t>(
                    (next.channels[i] * static_cast<unsigned>(group_dimmer) + 50U) / 100U);
            next.channels[local] = static_cast<std::uint8_t>((value * 255U + 50U) / 100U);
            if (!light.independent() && !light.separate() && light.channel_count() == 5U) {
                if (local < 3U)
                    next.channels[3] = next.channels[4] = 0U;
                else
                    next.channels[0] = next.channels[1] = next.channels[2] = 0U;
            }
            if (white_group)
                next.white_dimmer = 100U;
            else
                next.dimmer = 100U;
            if (!white_group)
                next.scheme = 0U;
            if (!light.separate() || white_group)
                derive_ct(light, next);
            const auto applied = light.update(next, true, static_cast<std::uint8_t>(local + 1U));
            if (applied != platform::PwmResult::success)
                return result(applied, output);
        }
        return output.format("{\"Channel%u\":%u}", static_cast<unsigned>(index),
                   static_cast<unsigned>(
                       (light.intent().channels[local] *
                               static_cast<unsigned>(light.separate() && local >= 3U
                                                         ? light.intent().white_dimmer
                                                         : light.intent().dimmer) +
                           127U) /
                       255U))
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    }
    if (color) {
        if (light.channel_count() < 2U ||
            (request.has_index && request.index != 1U && request.index != 2U))
            return CommandStatus::invalid;
        if (!request.is_query()) {
            const auto text = support::trim_ascii(request.payload);
            std::uint32_t selection = 0U;
            const bool cycle = text == "+" || text == "-";
            const bool numeric =
                text.size() <= 3U && support::parse_unsigned_decimal(text, 203U, selection);
            if (cycle && light.channel_count() < 3U)
                return CommandStatus::invalid;
            if (light.channel_count() >= 3U && next.palette_count != 0U && cycle) {
                selection = text == "+" ? next.palette_index + 1U
                                        : next.palette_index + next.palette_count - 1U;
                next.palette_index = static_cast<std::uint8_t>(selection % next.palette_count);
                next.channels = next.palette[next.palette_index];
            } else {
                if (cycle) {
                    selection = text == "+" ? std::min<unsigned>(12U, next.fixed_color_index + 1U)
                                            : std::max<unsigned>(1U, next.fixed_color_index - 1U);
                    if (!fixed_color(selection, light.channel_count(), next.channels))
                        return CommandStatus::invalid;
                } else if (!parse_color(text, light.channel_count(), next.channels))
                    return CommandStatus::invalid;
                if ((cycle || numeric) && selection >= 1U && selection <= 12U)
                    next.fixed_color_index = static_cast<std::uint8_t>(selection);
            }
            next.scheme = 0U;
            std::uint8_t peak = 0U;
            for (std::size_t i = 0U; i < light.channel_count(); ++i)
                peak = std::max(peak, next.channels[i]);
            if (!request.has_index || request.index == 1U)
                next.dimmer = static_cast<std::uint8_t>((peak * 100U + 127U) / 255U);
            if (peak != 0U)
                for (std::size_t i = 0U; i < light.channel_count(); ++i)
                    next.channels[i] = static_cast<std::uint8_t>(
                        (static_cast<unsigned>(next.channels[i]) * 255U + peak / 2U) / peak);
            if (light.separate() && (!request.has_index || request.index == 1U))
                next.white_dimmer = next.dimmer;
            derive_ct(light, next);
            const auto applied = light.update(next, true);
            if (applied != platform::PwmResult::success)
                return result(applied, output);
        }
        return color_state(light, output);
    }
    if (hsb) {
        if ((request.has_index && (request.index == 0U || request.index > 3U)) ||
            (light.independent() && !light.addressable()) || light.channel_count() < 3U)
            return CommandStatus::invalid;
        if (request.is_query())
            return request.has_index ? hsb_component_state(light, request.index, output)
                                     : hsb_state(light, output);
        std::array<std::uint16_t, 3> values{};
        std::uint8_t current_saturation = 0U;
        rgb_to_hs(next.channels, values[0], current_saturation);
        values[1] = static_cast<std::uint16_t>(scale_color(current_saturation, 255U, 100U));
        const auto rgb_peak = std::max({next.channels[0], next.channels[1], next.channels[2]});
        values[2] = static_cast<std::uint16_t>(std::min<std::uint32_t>(
            100U, (static_cast<std::uint32_t>(rgb_peak) * next.dimmer + 127U) / 255U));

        if (request.has_index) {
            std::uint32_t value = 0U;
            const auto component = static_cast<std::size_t>(request.index - 1U);
            const auto maximum = component == 0U ? 360U : 100U;
            if (!number(request, maximum, value))
                return CommandStatus::invalid;
            values[component] = static_cast<std::uint16_t>(value);
        } else {
            std::array<std::uint16_t, 3> parsed{};
            std::size_t parsed_count = 0U;
            if (!parse_hsb(request.payload, parsed, parsed_count))
                return CommandStatus::invalid;
            for (std::size_t i = 0U; i < parsed_count; ++i)
                values[i] = parsed[i];
        }

        std::array<std::uint8_t, 3> rgb{};
        hsb_to_rgb(values[0], static_cast<std::uint8_t>(scale_color(values[1], 100U, 255U)), rgb);
        for (std::size_t i = 0U; i < rgb.size(); ++i)
            next.channels[i] = rgb[i];
        if (!light.separate())
            for (std::size_t i = 3U; i < light.channel_count(); ++i)
                next.channels[i] = 0U;
        next.scheme = 0U;
        next.dimmer = static_cast<std::uint8_t>(values[2]);
        const auto applied = light.update(next, true, light.separate() ? 1U : 0U);
        if (applied != platform::PwmResult::success)
            return result(applied, output);
        return request.has_index ? hsb_component_state(light, request.index, output)
                                 : hsb_state(light, output);
    }
    const bool white_dimmer =
        dimmer && light.separate() && request.has_index && request.index == 2U;
    if (!white_dimmer && !scalar_index(request))
        return CommandStatus::invalid;
    if (ct) {
        if (light.independent() || (light.channel_count() != 2U && light.channel_count() != 5U))
            return CommandStatus::invalid;
        if (!request.is_query()) {
            std::uint32_t value = 0U;
            if (!number(request, 500U, value) || value < 153U)
                return CommandStatus::invalid;
            next.color_temperature = static_cast<std::uint16_t>(value);
            const auto offset = light.channel_count() == 2U ? 0U : 3U;
            const auto warm = (value - 153U) * 255U / 347U;
            if (!light.separate())
                next.channels.fill(0U);
            if (!light.separate())
                next.scheme = 0U;
            next.channels[offset] = static_cast<std::uint8_t>(255U - warm);
            next.channels[offset + 1U] = static_cast<std::uint8_t>(warm);
            const auto applied = light.update(next, true, light.separate() ? 4U : 0U);
            if (applied != platform::PwmResult::success)
                return result(applied, output);
        }
        return scalar(output, "CT", light.intent().color_temperature);
    }
    if (dimmer || speed) {
        if (!request.is_query()) {
            std::uint32_t value = 0U;
            if (dimmer) {
                if (!percent(request, white_dimmer ? next.white_dimmer : next.dimmer, value))
                    return CommandStatus::invalid;
            } else if (request.payload == "+")
                value = next.speed > 1U ? next.speed - 1U : 1U;
            else if (request.payload == "-")
                value = next.speed < 40U ? next.speed + 1U : 40U;
            else if (!number(request, 40U, value) || value == 0U)
                return CommandStatus::invalid;
            if (dimmer) {
                if (white_dimmer)
                    next.white_dimmer = static_cast<std::uint8_t>(value);
                else
                    next.dimmer = static_cast<std::uint8_t>(value);
                if (light.channel_count() == 1U)
                    next.channels[0] = 255U;
            } else
                next.speed = static_cast<std::uint8_t>(value);
            const auto applied = light.update(
                next, dimmer, light.separate() && dimmer ? (white_dimmer ? 4U : 1U) : 0U);
            if (applied != platform::PwmResult::success)
                return result(applied, output);
        }
        return scalar(output,
            white_dimmer ? "Dimmer2"
            : dimmer     ? "Dimmer"
                         : "Speed",
            white_dimmer ? light.intent().white_dimmer
            : dimmer     ? light.intent().dimmer
                         : light.intent().speed);
    }
    if (!request.is_query()) {
        const auto state = parse_command_state(request.payload);
        if (state == CommandState::invalid)
            return CommandStatus::invalid;
        bool& value = fade ? next.fade : next.gamma;
        value = state == CommandState::toggle ? !value : state == CommandState::on;
        const auto applied = light.update(next, false);
        if (applied != platform::PwmResult::success)
            return result(applied, output);
    }
    return output.format("{\"%s\":\"%s\"}", fade ? "Fade" : "LedTable",
               (fade ? light.intent().fade : light.intent().gamma) ? "ON" : "OFF")
               ? CommandStatus::handled
               : CommandStatus::failed;
}
} // namespace newmota::commands
