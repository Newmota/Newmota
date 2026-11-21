/*
  restart_commands.h - restart command interface for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "core/command/command.h"

namespace newmota {

class RestartController;

namespace commands {

/**
 * @brief Dispatches the Tasmota-compatible Restart command.
 * @return The handler status, or unknown when the request is unrelated.
 */
[[nodiscard]] CommandStatus dispatch_restart(
    const CommandRequest& request, RestartController& restart, CommandOutput& output);

} // namespace commands
} // namespace newmota
