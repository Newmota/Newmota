/*
  timer_commands.h - timer command dispatch for Newmota

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

namespace commands {
/** @brief Dispatches persisted Timer, Timers, coordinates and Sunrise commands. */
[[nodiscard]] CommandStatus dispatch_timers(
    const CommandRequest& request, Settings& settings, PowerService& power, CommandOutput& output);

/**
 * @brief Writes the global switch, solar settings, and all timers for the web editor.
 *
 * Shape: `{"Timers":"ON|OFF","TimeValid":true|false,"Latitude":"d.dddddd",
 * "Longitude":"d.dddddd","Sunrise":n,"List":[<Tasmota Timer fields>, ...]}` with one entry
 * per timer in index order. Timers fire only while `TimeValid` is true.
 */
[[nodiscard]] bool write_timers_overview(
    CommandOutput& output, const Settings& settings, bool time_valid);
} // namespace commands
} // namespace newmota
