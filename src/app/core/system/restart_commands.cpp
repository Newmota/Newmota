/*
  restart_commands.cpp - restart commands for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file restart_commands.cpp
 * @brief User restart command routing and update interlock reporting.
 */

#include "core/system/restart_commands.h"

#include "core/system/restart.h"
#include "newmota/platform/platform.h"
#include "support/number.h"
#include "support/text.h"

#include <cstdint>

namespace newmota::commands {
namespace {

/**
 * @brief Writes a Restart JSON response and returns the requested status.
 */
[[nodiscard]] CommandStatus write_result(
    CommandOutput& output, const char* result, const CommandStatus status) {
    return output.format("{\"Restart\":\"%s\"}", result) ? status : CommandStatus::failed;
}

/**
 * @brief Schedules a restart and translates its outcome to a command response.
 */
[[nodiscard]] CommandStatus schedule_restart(
    RestartController& restart, const RestartTarget target, CommandOutput& output) {
    switch (restart.schedule(target, platform::monotonic_milliseconds())) {
    case RestartRequestResult::scheduled:
        return write_result(output, target == RestartTarget::current ? "Restarting" : "Switching",
            CommandStatus::handled);
    case RestartRequestResult::unavailable:
        return write_result(output, "Safeboot unavailable", CommandStatus::failed);
    case RestartRequestResult::failed:
        return write_result(output, "Partition switch failed", CommandStatus::failed);
    case RestartRequestResult::busy:
        return write_result(output, "Restart pending", CommandStatus::failed);
    }
    return CommandStatus::failed;
}

} // namespace

/**
 * @brief Dispatches the Tasmota-compatible Restart command.
 */
CommandStatus dispatch_restart(
    const CommandRequest& request, RestartController& restart, CommandOutput& output) {
    if (!support::ascii_iequals(request.name, "restart")) {
        return CommandStatus::unknown;
    }

    std::uint32_t mode = 0;
    if (request.has_index || request.is_query() ||
        !support::parse_unsigned_decimal(request.payload, 99, mode)) {
        return write_result(output, "1 to restart", CommandStatus::handled);
    }

    switch (mode) {
    case 1:
        return schedule_restart(restart, RestartTarget::current, output);
    case 3:
        return schedule_restart(restart, RestartTarget::alternate_any, output);
    case 99:
        restart.restart_now();
    default:
        return write_result(output, "1 to restart", CommandStatus::handled);
    }
}

} // namespace newmota::commands
