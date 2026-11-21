/*
  display_commands.cpp - descriptor display control commands

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file display_commands.cpp
 * @brief Routes display status and controls to the same panel used by Berry and LVGL.
 */
#include "drivers/display/display_commands.h"

#include "newmota/platform/display.h"
#include "support/number.h"
#include "support/text.h"

namespace newmota::commands {
namespace {
namespace display = platform::display;

/** @brief Names the complete small command surface without accepting arbitrary prefixes. */
const char* command_name(std::string_view name) {
    constexpr const char* names[] = {"Display", "DisplayDimmer", "DisplayRotate", "DisplayInvert",
        "DisplayPower", "DisplayRefreshNow"};
    for (const auto* candidate : names)
        if (support::ascii_iequals(name, candidate))
            return candidate;
    return nullptr;
}

/** @brief Converts retained 8-bit brightness to effective percentage; power off reads zero. */
unsigned dimmer(const display::State& state) {
    return state.powered ? (static_cast<unsigned>(state.brightness) * 100U + 127U) / 255U : 0U;
}

/** @brief Reports a native failure without presenting it as an accepted control update. */
CommandStatus operation_error(CommandOutput& output, const char* name, display::Result result) {
    const char* reason = "Failed";
    switch (result) {
    case display::Result::unsupported:
        reason = "Unsupported";
        break;
    case display::Result::busy:
        reason = "Busy";
        break;
    case display::Result::timeout:
        reason = "Timeout";
        break;
    case display::Result::no_memory:
        reason = "No memory";
        break;
    default:
        break;
    }
    (void)output.format("{\"%s\":\"%s\"}", name, reason);
    return CommandStatus::failed;
}
} // namespace

CommandStatus dispatch_display(const CommandRequest& request, CommandOutput& output) {
    const auto* name = command_name(request.name);
    if (name == nullptr)
        return CommandStatus::unknown;
    if (request.has_index)
        return CommandStatus::invalid;

    const bool status = support::ascii_iequals(request.name, "Display");
    const bool refresh = support::ascii_iequals(request.name, "DisplayRefreshNow");
    const bool brightness = support::ascii_iequals(request.name, "DisplayDimmer");
    const bool rotate = support::ascii_iequals(request.name, "DisplayRotate");
    const bool invert = support::ascii_iequals(request.name, "DisplayInvert");
    std::uint32_t value = 0;
    auto power = CommandState::invalid;

    // RefreshNow is an action: an empty payload starts a full refresh, and '?' is invalid.
    if (status) {
        if (!request.is_query())
            return CommandStatus::invalid;
    } else if (refresh) {
        if (request.has_payload && !support::parse_unsigned_decimal(request.payload, 1U, value))
            return CommandStatus::invalid;
    } else if (!request.is_query()) {
        if (brightness || rotate || invert) {
            if (!support::parse_unsigned_decimal(
                    request.payload, brightness ? 100U : (rotate ? 3U : 1U), value))
                return CommandStatus::invalid;
        } else {
            power = parse_command_state(request.payload);
            if (power == CommandState::invalid)
                return CommandStatus::invalid;
        }
    }

    display::State state{};
    const auto* panel = display::info();
    if (panel == nullptr || !display::state(state)) {
        if (status) {
            (void)output.write("{\"Display\":{\"Available\":false,\"Ready\":false}}");
            return CommandStatus::handled;
        }
        (void)output.format("{\"%s\":\"Unavailable\"}", name);
        return CommandStatus::failed;
    }

    auto result = display::Result::success;
    if (refresh) {
        result = display::refresh(value != 0U);
    } else if (!request.is_query()) {
        if (brightness) {
            if (value == 0U) {
                result = display::power(false);
            } else {
                result =
                    display::brightness(static_cast<std::uint8_t>((value * 255U + 50U) / 100U));
                if (result == display::Result::success && !state.powered)
                    result = display::power(true);
            }
        } else if (rotate) {
            result = display::rotation(static_cast<std::uint8_t>(value));
        } else if (invert) {
            result = display::invert(value != 0U);
        } else {
            result = display::power(
                power == CommandState::toggle ? !state.powered : power == CommandState::on);
        }
    }
    if (result != display::Result::success)
        return operation_error(output, name, result);
    (void)display::state(state);

    if (status) {
        (void)output.write("{\"Display\":{\"Available\":true,\"Name\":");
        (void)output.json_string(panel->name);
        (void)output.format(",\"Ready\":%s,\"Width\":%u,\"Height\":%u,\"Dimmer\":%u,",
            state.ready ? "true" : "false", unsigned(panel->width), unsigned(panel->height),
            dimmer(state));
        (void)output.format("\"Rotate\":%u,\"Invert\":%u,\"Power\":\"%s\"}}",
            unsigned(state.rotation), unsigned(state.inverted), state.powered ? "ON" : "OFF");
    } else if (refresh) {
        (void)output.write("{\"DisplayRefreshNow\":\"Accepted\"}");
    } else if (brightness || rotate || invert) {
        (void)output.format("{\"%s\":%u}", name,
            brightness ? dimmer(state)
                       : (rotate ? unsigned(state.rotation) : unsigned(state.inverted)));
    } else {
        (void)output.format("{\"DisplayPower\":\"%s\"}", state.powered ? "ON" : "OFF");
    }
    return CommandStatus::handled;
}
} // namespace newmota::commands
