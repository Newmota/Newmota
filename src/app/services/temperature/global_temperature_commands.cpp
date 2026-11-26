/*
  global_temperature_commands.cpp - Volatile manual global-temperature commands

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  CmndGlobalTemp/CharToFloat reference: Tasmota support_command.ino and support.ino,
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
*/

/** @file global_temperature_commands.cpp
 * @brief Implements manual Celsius/Fahrenheit input without modifying physical sensor samples.
 */
#include "services/temperature/global_temperature_commands.h"
#include "services/temperature/global_temperature.h"
#include "services/settings/settings.h"
#include "newmota/platform/platform.h"
#include "support/number.h"
#include "support/text.h"
#include <cmath>

namespace newmota::commands {
namespace {
/** @brief Matches the source 23-byte decimal prefix and eight fractional-digit input.
 * Nonnumeric input becomes zero, trailing text is ignored, and exponent notation is
 * not expanded. Unrepresentable integer prefixes are rejected instead of emulating
 * upstream atoi overflow; every defined in-range source manual value is representable.
 */
bool manual_value(std::string_view payload, bool fahrenheit, std::int32_t& celsius) {
    auto text = payload.substr(0, 23);
    while (!text.empty() && support::ascii_is_space(text.front()))
        text.remove_prefix(1);
    const bool negative = !text.empty() && text.front() == '-';
    if (!text.empty() && (text.front() == '-' || text.front() == '+'))
        text.remove_prefix(1);
    std::uint32_t whole{};
    while (!text.empty() && text.front() >= '0' && text.front() <= '9') {
        if (whole > 100000000)
            return false;
        whole = whole * 10 + unsigned(text.front() - '0');
        text.remove_prefix(1);
    }
    std::uint32_t fraction{};
    unsigned decimals{};
    if (!text.empty() && text.front() == '.') {
        text.remove_prefix(1);
        while (decimals < 8 && !text.empty() && text.front() >= '0' && text.front() <= '9') {
            fraction = fraction * 10 + unsigned(text.front() - '0');
            ++decimals;
            text.remove_prefix(1);
        }
    }
    float right = static_cast<float>(fraction);
    while (decimals--)
        right /= 10.0f;
    float value = static_cast<float>(whole) + right;
    if (negative)
        value = -value;
    if (fahrenheit)
        value = (value - 32.0f) / 1.8f;
    if (value < -50.0f || value > 100.0f)
        return false;
    celsius = static_cast<std::int32_t>(std::lround(value * 1000.0f));
    return true;
}
} // namespace

CommandStatus dispatch_global_temperature(
    const CommandRequest& request, Settings& settings, CommandOutput& output) {
    if (!support::ascii_iequals(request.name, "GlobalTemp"))
        return CommandStatus::unknown;
    auto& owner = global_temperature();
    if (request.has_index && request.index == 2) {
        if (!request.is_query()) {
            std::uint32_t selector{};
            if (support::parse_unsigned_decimal(request.payload, 250, selector)) {
                const auto result = settings.set_global_temperature_source(selector);
                if (result == SettingsUpdateResult::failed)
                    return CommandStatus::failed;
                if (result == SettingsUpdateResult::invalid)
                    return CommandStatus::invalid;
                /** Even an unchanged valid selector releases manual precedence, as source does. */
                owner.configure_source(static_cast<std::uint8_t>(selector));
            }
        }
        return output.format("{\"GlobalTemp2\":%u}", unsigned(settings.global_temperature_source()))
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    }
    const auto now = platform::monotonic_milliseconds();
    if (!request.is_query()) {
        std::uint8_t fahrenheit{};
        (void)settings.option(8, fahrenheit);
        std::int32_t value{};
        if (manual_value(request.payload, fahrenheit != 0, value))
            (void)owner.set_manual(value, now);
    }
    std::int32_t celsius{};
    if (!owner.value(now, celsius))
        return output.write("{\"GlobalTemp\":null}") ? CommandStatus::handled
                                                     : CommandStatus::failed;
    const auto magnitude = celsius < 0 ? -std::int64_t(celsius) : std::int64_t(celsius);
    const auto tenths = (magnitude + 50) / 100;
    return output.format("{\"GlobalTemp\":%s%s.%u}", celsius < 0 ? "-" : "",
               newmota::support::decimal(std::int64_t(tenths / 10)).text, unsigned(tenths % 10))
               ? CommandStatus::handled
               : CommandStatus::failed;
}
} // namespace newmota::commands
