/*
  settings_commands.h - settings command interface for Newmota

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
class PowerService;
class RestartController;

namespace commands {

/**
 * @brief Dispatches settings and Wi-Fi credential commands.
 * @return The handler status, or unknown when the request is unrelated.
 */
[[nodiscard]] CommandStatus dispatch_settings(const CommandRequest& request, Settings& settings,
    RestartController& restart, PowerService& power, CommandOutput& output);

} // namespace commands
} // namespace newmota
