/*
  display_commands.h - descriptor display command interface

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file display_commands.h
 * @brief Shared display commands over the retained native panel owner.
 */
#pragma once

#include "core/command/command.h"

namespace newmota::commands {
/** @brief Applies or queries descriptor display controls through any command transport.
 * @param request Parsed unindexed command and payload.
 * @param output Destination for the bounded JSON response.
 * @return unknown for another command, invalid for bad arguments, or the operation result.
 */
[[nodiscard]] CommandStatus dispatch_display(const CommandRequest& request, CommandOutput& output);
} // namespace newmota::commands
