/*
  reset_commands.h - reset command interface for Newmota

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
class Settings;
class BootCounter;

namespace commands {

/**
 * @brief Dispatches the Tasmota-compatible Reset command.
 * @return The handler status, or unknown when the request is unrelated.
 */
[[nodiscard]] CommandStatus dispatch_reset(const CommandRequest& request, Settings& settings,
    RestartController& restart, BootCounter* boot_counter, CommandOutput& output);

} // namespace commands
} // namespace newmota
