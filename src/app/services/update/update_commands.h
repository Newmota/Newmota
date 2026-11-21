/*
  update_commands.h - Tasmota-compatible URL update commands

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "core/command/command.h"

namespace newmota {

class UrlUpdateService;

namespace commands {

/** @brief Dispatches `Upgrade 1` and reports current URL-pull progress. */
[[nodiscard]] CommandStatus dispatch_url_update(
    const CommandRequest& request, UrlUpdateService& update, CommandOutput& output);

} // namespace commands
} // namespace newmota
