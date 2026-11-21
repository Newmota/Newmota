/*
  led_commands.cpp - Tasmota LedState, LedPower, and LedMask commands

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Source reference: Tasmota support_command.ino (CmndLedPower, CmndLedState, CmndLedMask),
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
*/

#include "services/power/led_commands.h"

#include "services/power/power_service.h"
#include "services/settings/settings.h"
#include "services/settings/state_text.h"
#include "support/number.h"
#include "support/text.h"

namespace newmota::commands {
namespace {
constexpr std::uint8_t kMaximumLeds = 4U;
constexpr std::uint8_t kLedStateOptions = 8U;

CommandStatus status(const SettingsUpdateResult result) {
    switch (result) {
    case SettingsUpdateResult::saved:
    case SettingsUpdateResult::unchanged:
    case SettingsUpdateResult::pending:
        return CommandStatus::handled;
    case SettingsUpdateResult::invalid:
        return CommandStatus::invalid;
    case SettingsUpdateResult::failed:
        return CommandStatus::failed;
    }
    return CommandStatus::failed;
}

/** @brief Parses decimal or 0x-prefixed hexadecimal LedMask values. */
bool parse_mask(std::string_view text, std::uint16_t& mask) {
    text = support::trim_ascii(text);
    std::uint32_t value = 0U;
    if (text.size() > 2U && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2U);
        if (text.empty() || text.size() > 4U)
            return false;
        for (const char digit : text) {
            const int nibble = digit >= '0' && digit <= '9'   ? digit - '0'
                               : digit >= 'a' && digit <= 'f' ? digit - 'a' + 10
                               : digit >= 'A' && digit <= 'F' ? digit - 'A' + 10
                                                              : -1;
            if (nibble < 0)
                return false;
            value = (value << 4U) | static_cast<std::uint32_t>(nibble);
        }
    } else if (!support::parse_unsigned_decimal(text, 0xFFFFU, value)) {
        return false;
    }
    mask = static_cast<std::uint16_t>(value);
    return true;
}

/** @brief Parses a LedPwmOn/LedPwmOff level, clamping like Tasmota (negative 0, above 255). */
bool parse_level(std::string_view text, std::uint8_t& level) {
    text = support::trim_ascii(text);
    bool negative = false;
    if (!text.empty() && text.front() == '-') {
        negative = true;
        text.remove_prefix(1U);
    }
    std::uint32_t value = 0U;
    if (!support::parse_unsigned_decimal(text, 0xFFFFFFFFU, value))
        return false;
    level = negative ? 0U : static_cast<std::uint8_t>(value > 255U ? 255U : value);
    return true;
}
} // namespace

CommandStatus dispatch_leds(
    const CommandRequest& request, Settings& settings, PowerService& power, CommandOutput& output) {
    auto& leds = power.leds();
    if (support::ascii_iequals(request.name, "LedPower")) {
        const auto index = request.has_index ? request.index : 1U;
        if (index < 1U || index > kMaximumLeds)
            return CommandStatus::invalid;
        auto state = settings.power_settings().led_state;
        if (!request.is_query()) {
            const auto action = parse_command_state(request.payload);
            if (action == CommandState::invalid)
                return CommandStatus::invalid;
            state = leds.set_power(static_cast<std::uint8_t>(index),
                action == CommandState::off  ? 0U
                : action == CommandState::on ? 1U
                                             : 2U,
                state);
            const auto result = status(settings.set_led_state(state));
            if (result != CommandStatus::handled)
                return result;
        }
        const auto reported = leds.has_link_led() ? index : 1U;
        return output.format("{\"LedPower%u\":", static_cast<unsigned>(reported)) &&
                       output.json_string(
                           state_text(leds.power(static_cast<std::uint8_t>(reported), state))) &&
                       output.write("}")
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    }
    if (support::ascii_iequals(request.name, "LedPwmMode")) {
        const auto requested = request.has_index ? request.index : 1U;
        if (requested < 1U || requested > kMaximumLeds)
            return CommandStatus::invalid;
        const auto index = leds.has_link_led() ? requested : 1U;
        const auto& current = settings.power_settings();
        auto mask = current.led_pwm_mask;
        if (!request.is_query()) {
            const auto action = parse_command_state(request.payload);
            if (action == CommandState::invalid)
                return CommandStatus::invalid;
            const auto bit = static_cast<std::uint8_t>(1U << (index - 1U));
            mask = action == CommandState::off  ? static_cast<std::uint8_t>(mask & ~bit)
                   : action == CommandState::on ? static_cast<std::uint8_t>(mask | bit)
                                                : static_cast<std::uint8_t>(mask ^ bit);
            const auto result =
                status(settings.set_led_pwm(mask, current.led_pwm_on, current.led_pwm_off));
            if (result != CommandStatus::handled)
                return result;
        }
        return output.format("{\"LedPwmMode%u\":", static_cast<unsigned>(index)) &&
                       output.json_string(state_text(
                           LedService::pwm_mode(static_cast<std::uint8_t>(index), mask))) &&
                       output.write("}")
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    }
    if (request.has_index)
        return support::ascii_iequals(request.name, "LedState") ||
                       support::ascii_iequals(request.name, "LedPwmOn") ||
                       support::ascii_iequals(request.name, "LedPwmOff") ||
                       support::ascii_iequals(request.name, "LedMask")
                   ? CommandStatus::invalid
                   : CommandStatus::unknown;
    if (support::ascii_iequals(request.name, "LedState")) {
        if (!request.is_query()) {
            std::uint32_t value = 0U;
            if (!support::parse_unsigned_decimal(
                    support::trim_ascii(request.payload), kLedStateOptions - 1U, value))
                return CommandStatus::invalid;
            const auto result = status(settings.set_led_state(static_cast<std::uint8_t>(value)));
            if (result != CommandStatus::handled)
                return result;
            leds.state_changed(static_cast<std::uint8_t>(value));
        }
        return output.format(
                   "{\"LedState\":%u}", static_cast<unsigned>(settings.power_settings().led_state))
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    }
    const bool pwm_on = support::ascii_iequals(request.name, "LedPwmOn");
    if (pwm_on || support::ascii_iequals(request.name, "LedPwmOff")) {
        const auto& current = settings.power_settings();
        if (!request.is_query()) {
            std::uint8_t level = 0U;
            if (!parse_level(request.payload, level))
                return CommandStatus::invalid;
            const auto result = status(settings.set_led_pwm(current.led_pwm_mask,
                pwm_on ? level : current.led_pwm_on, pwm_on ? current.led_pwm_off : level));
            if (result != CommandStatus::handled)
                return result;
        }
        const auto& saved = settings.power_settings();
        return output.format("{\"%s\":%u}", pwm_on ? "LedPwmOn" : "LedPwmOff",
                   static_cast<unsigned>(pwm_on ? saved.led_pwm_on : saved.led_pwm_off))
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    }
    if (support::ascii_iequals(request.name, "LedMask")) {
        if (!request.is_query()) {
            std::uint16_t mask = 0U;
            if (!parse_mask(request.payload, mask))
                return CommandStatus::invalid;
            const auto result = status(settings.set_led_mask(mask));
            if (result != CommandStatus::handled)
                return result;
        }
        const auto mask = settings.power_settings().led_mask;
        return output.format("{\"LedMask\":\"%u (0x%04X)\"}", static_cast<unsigned>(mask),
                   static_cast<unsigned>(mask))
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    }
    return CommandStatus::unknown;
}
} // namespace newmota::commands
