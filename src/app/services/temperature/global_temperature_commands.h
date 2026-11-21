/*
  global_temperature_commands.h - Shared global-temperature command entry

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  GlobalTemp reference: Tasmota support_command.ino, Theo Arends, GPL version 3 or later.
*/

/** @file global_temperature_commands.h
 * @brief Exposes the optional manual global-temperature lane through the common dispatcher.
 */
#pragma once
#include "core/command/command.h"

namespace newmota {
class Settings;

namespace commands {
/** @brief Routes manual GlobalTemp and durable GlobalTemp2 field selection.
 * @param request Normalized shared command, independent of the caller transport.
 * @param settings Supplies SO8 units and the atomic selector transaction; manual values are
 * volatile.
 * @param output Receives canonical Celsius at one decimal, or null when unavailable.
 * @return unknown for unrelated commands; failed when a selector cannot be durably saved.
 */
CommandStatus dispatch_global_temperature(
    const CommandRequest& request, Settings& settings, CommandOutput& output);
} // namespace commands
} // namespace newmota
