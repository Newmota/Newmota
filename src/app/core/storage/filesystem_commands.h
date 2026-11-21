/*
  filesystem_commands.h - filesystem command interface for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "core/command/command.h"

namespace newmota::commands {

/**
 * @brief Dispatches pseudo-shell filesystem commands.
 * @return The handler status, or unknown when the request is unrelated.
 */
[[nodiscard]] CommandStatus dispatch_filesystem(
    const CommandRequest& request, CommandOutput& output);

} // namespace newmota::commands
