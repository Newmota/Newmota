/*
  power_commands.h - power command interface for Newmota

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

namespace commands {

/** @brief Dispatches Tasmota-compatible Power and indexed Power commands. */
[[nodiscard]] CommandStatus dispatch_power(
    const CommandRequest& request, PowerService& power, CommandOutput& output);

} // namespace commands
} // namespace newmota
