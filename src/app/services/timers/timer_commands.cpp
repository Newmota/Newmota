/*
  timer_commands.cpp - persisted civil timer commands for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/timers/timer_commands.h"
#include "services/settings/settings.h"
#include "services/power/power_service.h"
#include "support/json.h"
#include "support/number.h"
#include "support/text.h"
#include <array>
#include <cstdint>
#include <string_view>

namespace newmota::commands {
namespace {
/** @brief Converts a persistent transaction outcome into a command result. */
CommandStatus status(const SettingsUpdateResult result) {
    if (result == SettingsUpdateResult::invalid)
        return CommandStatus::invalid;
    if (result == SettingsUpdateResult::failed)
        return CommandStatus::failed;
    return CommandStatus::handled;
}

/** @brief Parses the Tasmota timer Time representation after selecting Mode. */
bool parse_time(std::string_view text, const std::uint8_t mode, std::uint16_t& result) {
    bool negative = false;
    if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    const auto separator = text.find(':');
    if (separator == std::string_view::npos || (negative && mode == 0))
        return false;
    std::uint32_t hours = 0, minutes = 0;
    if (!support::parse_unsigned_decimal(text.substr(0, separator), mode ? 11U : 23U, hours) ||
        !support::parse_unsigned_decimal(text.substr(separator + 1), 59, minutes))
        return false;
    result = static_cast<std::uint16_t>(hours * 60 + minutes + (negative ? 720 : 0));
    return true;
}

/** @brief Applies a complete flat partial update to a temporary entry. */
bool parse_timer(const std::string_view payload, TimerSettings::Entry& timer) {
    // Nine members at most; a longer object holds an unknown or repeated key.
    std::array<support::JsonToken, 1 + 2 * 9> tokens{};
    support::JsonReader json{payload, tokens.data(), tokens.size()};
    std::size_t root = 0;
    if (!json.parse(root) || json.token(root).kind != support::JsonKind::object)
        return false;
    std::uint16_t seen = 0;
    std::array<char, 8> time_text{};
    std::size_t time_length = 0;
    for (auto key = json.first(root); key < json.token_count();) {
        const auto value = json.next(root, key);
        if (value >= json.token_count())
            return false;
        constexpr std::array<std::string_view, 9> names{
            "Enable", "Mode", "Time", "Window", "Days", "Repeat", "Output", "Action", "Arm"};
        std::size_t index = 0;
        while (index < names.size() && !support::ascii_iequals(json.text(key), names[index]))
            ++index;
        if (index == names.size())
            return false;
        if (index == 8)
            index = 0;
        const auto bit = static_cast<std::uint16_t>(1U << index);
        if ((seen & bit) != 0)
            return false;
        seen |= bit;
        if (index == 2) {
            if (!support::json_copy_string(
                    json, value, time_text.data(), time_text.size(), time_length))
                return false;
        } else if (index == 4) {
            std::array<char, 8> days{};
            std::size_t length = 0;
            if (!support::json_copy_string(json, value, days.data(), days.size(), length) ||
                length != 7)
                return false;
            timer.days = 0;
            for (std::size_t day = 0; day < length; ++day)
                if (days[day] != '0' && days[day] != '-')
                    timer.days |= 1U << day;
        } else {
            std::uint32_t number = 0;
            if (!support::json_decimal(json, value, 65535, number))
                return false;
            switch (index) {
            case 0:
                timer.enabled = number != 0;
                break;
            case 1:
                if (number > 2)
                    return false;
                timer.mode = static_cast<std::uint8_t>(number);
                break;
            case 3:
                if (number > 15)
                    return false;
                timer.window = static_cast<std::uint8_t>(number);
                break;
            case 5:
                timer.repeat = number != 0;
                break;
            case 6:
                if (number < 1 || number > 16)
                    return false;
                timer.output = static_cast<std::uint8_t>(number);
                break;
            case 7:
                if (number > 3)
                    return false;
                timer.action = static_cast<std::uint8_t>(number);
                break;
            default:
                return false;
            }
        }
        key = json.next(root, value);
    }
    return ((seen & (1U << 2)) == 0 ||
               parse_time({time_text.data(), time_length}, timer.mode, timer.time)) &&
           timer.valid();
}

/** @brief Writes one timer's Tasmota fields as a JSON object. */
bool write_timer_fields(CommandOutput& output, const TimerSettings::Entry& timer) {
    std::array<char, 8> days{};
    for (unsigned day = 0; day < 7; ++day)
        days[day] = (timer.days & (1U << day)) ? '1' : '0';
    const bool negative = timer.mode != 0 && timer.time >= 720;
    const auto minutes = static_cast<unsigned>(negative ? timer.time - 720 : timer.time);
    return output.format("{\"Enable\":%u,\"Mode\":%u,\"Time\":\"%s%02u:%02u\","
                         "\"Window\":%u,\"Days\":\"%s\",\"Repeat\":%u,\"Output\":%u,\"Action\":%u}",
        timer.enabled, timer.mode, negative ? "-" : "", minutes / 60U, minutes % 60U, timer.window,
        days.data(), timer.repeat, timer.output, timer.action);
}

/** @brief Writes one bounded complete Timer response. */
bool write_timer(CommandOutput& output, const unsigned index, const TimerSettings::Entry& timer) {
    return output.format("{\"Timer%u\":", index) && write_timer_fields(output, timer) &&
           output.write("}");
}

/** @brief Writes a signed coordinate in millionths of a degree as a decimal string. */
bool write_coordinate(CommandOutput& output, const std::int32_t value) {
    const auto magnitude = static_cast<unsigned>(value < 0 ? -value : value);
    return output.format(
        "\"%s%u.%06u\"", value < 0 ? "-" : "", magnitude / 1000000U, magnitude % 1000000U);
}

/** @brief Parses precise decimal degrees without libc float formatting or rounding drift. */
bool coordinate(std::string_view text, const std::int32_t maximum, std::int32_t& result) {
    bool negative = false;
    if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    const auto dot = text.find('.');
    std::uint32_t degrees = 0;
    if (!support::parse_unsigned_decimal(
            text.substr(0, dot), static_cast<std::uint32_t>(maximum), degrees))
        return false;
    std::uint32_t fraction = 0;
    if (dot != std::string_view::npos) {
        const auto part = text.substr(dot + 1);
        if (part.empty() || part.size() > 6 ||
            !support::parse_unsigned_decimal(part, 999999, fraction))
            return false;
        for (auto size = part.size(); size < 6; ++size)
            fraction *= 10;
    }
    const auto magnitude = degrees * 1000000U + fraction;
    if (magnitude > static_cast<std::uint32_t>(maximum) * 1000000U)
        return false;
    result =
        negative ? -static_cast<std::int32_t>(magnitude) : static_cast<std::int32_t>(magnitude);
    return true;
}
} // namespace

bool write_timers_overview(CommandOutput& output, const Settings& settings, const bool time_valid) {
    const auto& policy = settings.timer_settings();
    if (!output.format("{\"Timers\":\"%s\",\"TimeValid\":%s,\"Latitude\":",
            policy.enabled ? "ON" : "OFF", time_valid ? "true" : "false") ||
        !write_coordinate(output, policy.latitude) || !output.write(",\"Longitude\":") ||
        !write_coordinate(output, policy.longitude) ||
        !output.format(",\"Sunrise\":%u,\"List\":[", policy.sunrise)) {
        return false;
    }
    for (std::size_t index = 0; index < policy.entries.size(); ++index) {
        if ((index != 0U && !output.write(",")) ||
            !write_timer_fields(output, policy.entries[index])) {
            return false;
        }
    }
    return output.write("]}");
}

CommandStatus dispatch_timers(
    const CommandRequest& request, Settings& settings, PowerService& power, CommandOutput& output) {
    if (support::ascii_iequals(request.name, "timer")) {
        if (!request.has_index || request.index < 1 || request.index > TimerSettings::kCount)
            return CommandStatus::invalid;
        if (!request.is_query()) {
            auto candidate = settings.timer_settings();
            auto& timer = candidate.entries[request.index - 1];
            std::uint32_t copy = 0;
            if (support::parse_unsigned_decimal(request.payload, TimerSettings::kCount, copy))
                timer = copy == 0 ? TimerSettings::Entry{} : candidate.entries[copy - 1];
            else if (!parse_timer(request.payload, timer))
                return CommandStatus::invalid;
            bool on = false;
            if (timer.enabled && timer.action < 3 && !power.state(timer.output, on))
                return CommandStatus::invalid;
            const auto result = status(settings.set_timer_settings(candidate));
            if (result != CommandStatus::handled)
                return result;
        }
        return write_timer(
                   output, request.index, settings.timer_settings().entries[request.index - 1])
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    }
    const bool timers = support::ascii_iequals(request.name, "timers");
    const bool latitude = support::ascii_iequals(request.name, "latitude");
    const bool longitude = support::ascii_iequals(request.name, "longitude");
    const bool sunrise = support::ascii_iequals(request.name, "sunrise");
    if (!timers && !latitude && !longitude && !sunrise)
        return CommandStatus::unknown;
    if (request.has_index)
        return CommandStatus::invalid;
    if (!request.is_query()) {
        auto candidate = settings.timer_settings();
        if (timers) {
            const auto value = parse_command_state(request.payload);
            if (value == CommandState::invalid)
                return CommandStatus::invalid;
            candidate.enabled =
                value == CommandState::toggle ? !candidate.enabled : value == CommandState::on;
        } else if (sunrise) {
            std::uint32_t value = 0;
            if (!support::parse_unsigned_decimal(request.payload, 3, value))
                return CommandStatus::invalid;
            candidate.sunrise = static_cast<std::uint8_t>(value);
        } else if (!coordinate(request.payload, latitude ? 90 : 180,
                       latitude ? candidate.latitude : candidate.longitude))
            return CommandStatus::invalid;
        const auto result = status(settings.set_timer_settings(candidate));
        if (result != CommandStatus::handled)
            return result;
    }
    const auto& policy = settings.timer_settings();
    bool written = false;
    if (timers)
        written = output.format("{\"Timers\":\"%s\"}", policy.enabled ? "ON" : "OFF");
    else if (sunrise)
        written = output.format("{\"Sunrise\":%u}", policy.sunrise);
    else {
        const auto value = latitude ? policy.latitude : policy.longitude;
        const auto magnitude = static_cast<unsigned>(value < 0 ? -value : value);
        written = output.format("{\"%s\":%s%u.%06u}", latitude ? "Latitude" : "Longitude",
            value < 0 ? "-" : "", magnitude / 1000000U, magnitude % 1000000U);
    }
    return written ? CommandStatus::handled : CommandStatus::failed;
}
} // namespace newmota::commands
