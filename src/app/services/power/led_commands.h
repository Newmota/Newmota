/*
  led_commands.h - Tasmota LedState, LedPower, and LedMask commands

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "core/command/command.h"

namespace newmota {
class PowerService;
class Settings;

namespace commands {
/** @brief Dispatches LedState, LedPower<n>, and LedMask. */
[[nodiscard]] CommandStatus dispatch_leds(
    const CommandRequest& request, Settings& settings, PowerService& power, CommandOutput& output);
} // namespace commands
} // namespace newmota
