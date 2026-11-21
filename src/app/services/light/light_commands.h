/*
  light_commands.h - portable PWM and Light behavior for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file light_commands.h
 * @brief Portable PWM and Light behavior for Newmota.
 */

#pragma once
#include "core/command/command.h"

namespace newmota {
class LightService;

namespace commands {
[[nodiscard]] CommandStatus dispatch_light(
    const CommandRequest& request, LightService& light, CommandOutput& output);
}
} // namespace newmota
