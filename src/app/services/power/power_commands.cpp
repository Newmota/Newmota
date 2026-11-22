/*
  power_commands.cpp - power commands for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Power source reference: Tasmota support_command.ino,
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
*/

/** @file power_commands.cpp
 * @brief Routes common Power commands and explicit caller provenance.
 */
#include "services/power/power_commands.h"

#include "services/power/power_service.h"
#include "support/text.h"
#include "services/settings/state_text.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace newmota::commands {
namespace {

/** @brief Maps real shared command transports; unsupported origins remain source zero. */
PowerOperationSource power_source(CommandSource source) {
    switch (source) {
    case CommandSource::serial:
        return PowerOperationSource::serial;
    case CommandSource::web_console:
        return PowerOperationSource::web_console;
    case CommandSource::http:
        return PowerOperationSource::web_command;
    case CommandSource::timer:
        return PowerOperationSource::timer;
    case CommandSource::berry:
        return PowerOperationSource::berry;
#if USE_MQTT
    case CommandSource::mqtt:
        return PowerOperationSource::mqtt;
#endif
#if USE_BACKLOG
    case CommandSource::backlog:
        return PowerOperationSource::backlog;
#endif
    default:
        return PowerOperationSource::unspecified;
    }
}

/** @brief Converts Tasmota's five Power payloads into a power action. */
[[nodiscard]] bool power_action(
    const std::string_view payload, const bool allow_blink, PowerAction& action) {
    const auto value = support::trim_ascii(payload);
    if (allow_blink && (value == "3" || support::ascii_iequals(value, "blink"))) {
        action = PowerAction::blink;
        return true;
    }
    if (allow_blink && (value == "4" || support::ascii_iequals(value, "blinkoff") ||
                           support::ascii_iequals(value, "blinkstop"))) {
        action = PowerAction::blink_stop;
        return true;
    }
    switch (parse_command_state(value)) {
    case CommandState::off:
        action = PowerAction::off;
        return true;
    case CommandState::on:
        action = PowerAction::on;
        return true;
    case CommandState::toggle:
        action = PowerAction::toggle;
        return true;
    case CommandState::invalid:
        return false;
    }
    return false;
}

/** @brief Writes a Tasmota-style Power response with an explicit state word. */
[[nodiscard]] bool write_power_word(CommandOutput& output, const PowerService& power,
    const std::uint8_t instance, const std::string_view word) {
    const bool prefix = power.endpoint_count() == 1U && instance == 1U
                            ? output.write("{\"POWER\":")
                            : output.format("{\"POWER%u\":", unsigned(instance));
    return prefix && output.json_string(word) && output.write("}");
}

/** @brief Writes one Tasmota-style logical power state response. */
[[nodiscard]] bool write_power_state(
    CommandOutput& output, const PowerService& power, const std::uint8_t instance, const bool on) {
    return write_power_word(output, power, instance, state_text(on));
}

/** @brief Writes every configured relay state for a Power0 operation. */
[[nodiscard]] bool write_all_power_states(CommandOutput& output, const PowerService& power) {
    if (!output.write("{")) {
        return false;
    }
    bool first = true;
    for (std::size_t index = 0; index < BasicIoDriver::kMaximumRelays; ++index) {
        bool on = false;
        const auto instance = static_cast<std::uint8_t>(index + 1U);
        if (!power.state(instance, on)) {
            continue;
        }
        if ((!first && !output.write(",")) ||
            !(power.endpoint_count() == 1U && instance == 1U
                    ? output.write("\"POWER\":")
                    : output.format("\"POWER%u\":", unsigned(instance))) ||
            !output.json_string(state_text(on)))
            return false;
        first = false;
    }
    return !first && output.write("}");
}

} // namespace

/** @brief Dispatches Tasmota-compatible Power and indexed Power commands. */
CommandStatus dispatch_power(
    const CommandRequest& request, PowerService& power, CommandOutput& output) {
    if (!support::ascii_iequals(request.name, "power")) {
        return CommandStatus::unknown;
    }
    if (request.has_index && request.index > BasicIoDriver::kMaximumRelays) {
        return CommandStatus::invalid;
    }
    const std::uint8_t instance = request.has_index ? static_cast<std::uint8_t>(request.index) : 1U;
    if (instance == 0U) {
        if (power.endpoint_count() == 0U) {
            return CommandStatus::invalid;
        }
        if (!request.is_query()) {
            PowerAction action{};
            if (!power_action(request.payload, false, action)) {
                return CommandStatus::invalid;
            }
            const auto result = power.set_all(action, power_source(request.source));
            if (result == PowerChangeResult::unavailable) {
                return CommandStatus::invalid;
            }
            if (result == PowerChangeResult::failed) {
                return CommandStatus::failed;
            }
        }
        return write_all_power_states(output, power) ? CommandStatus::handled
                                                     : CommandStatus::failed;
    }

    bool on = false;
    if (!power.state(instance, on)) {
        return CommandStatus::invalid;
    }
    if (!request.is_query()) {
        PowerAction action{};
        if (!power_action(request.payload, true, action)) {
            return CommandStatus::invalid;
        }
        const auto result = power.set(instance, action, power_source(request.source));
        if (result == PowerChangeResult::failed) {
            return CommandStatus::failed;
        }
        if (result == PowerChangeResult::unavailable || !power.state(instance, on)) {
            return CommandStatus::invalid;
        }
        if (action == PowerAction::blink && power.blinking(instance)) {
            return write_power_word(output, power, instance, "BLINK") ? CommandStatus::handled
                                                                      : CommandStatus::failed;
        }
        if (action == PowerAction::blink_stop) {
            return write_power_word(output, power, instance, "BLINKOFF") ? CommandStatus::handled
                                                                         : CommandStatus::failed;
        }
    }
    return write_power_state(output, power, instance, on) ? CommandStatus::handled
                                                          : CommandStatus::failed;
}

} // namespace newmota::commands
