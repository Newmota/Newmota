/*
  time_commands.h - time command interface for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "core/command/command.h"

namespace newmota {

class Settings;
class TimeService;

namespace commands {

/**
 * @brief Dispatches Tasmota-compatible Time and timezone commands.
 * @return The handler status, or unknown when the request is unrelated.
 */
[[nodiscard]] CommandStatus dispatch_time(
    const CommandRequest& request, Settings& settings, TimeService& time, CommandOutput& output);

/**
 * @brief Writes STATE's Time field (and Epoch in Time format 2) in the configured format, as
 * Tasmota's ResponseAppendTime does.
 */
[[nodiscard]] bool write_state_time(
    CommandOutput& output, const Settings& settings, const TimeService& time, std::uint32_t now);

} // namespace commands
} // namespace newmota
