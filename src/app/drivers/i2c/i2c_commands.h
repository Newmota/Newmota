/*
  i2c_commands.h - shared I2C diagnostic commands for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file i2c_commands.h
 * @brief Shared I2C diagnostic command interface.
 */

#pragma once

#include "core/command/command.h"

namespace newmota::commands {
/**
 * @brief Scans active shared buses for I2CScan.
 * @param request Parsed command; numeric index and payload are ignored.
 * @param output Response sink for the complete scan result.
 * @return unknown for another command, failed if the response exceeds its limit, or handled
 *         for a completed scan, including bus-level diagnostics.
 */
[[nodiscard]] CommandStatus dispatch_i2c(const CommandRequest& request, CommandOutput& output);
} // namespace newmota::commands
