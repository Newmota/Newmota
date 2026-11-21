/*
  hardware_commands.h - GPIO, Module, and Template commands for Newmota

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

namespace commands {

/**
 * @brief Dispatches application-only GPIO, Module, and Template commands.
 * @return The handler status, or unknown when the request is unrelated.
 */
[[nodiscard]] CommandStatus dispatch_hardware(const CommandRequest& request, Settings& settings,
    RestartController& restart, CommandOutput& output);

/**
 * @brief Writes the static editor catalog: architecture, implemented functions, and modules.
 *
 * `Functions` rows are `[first, instances, requirements, name]`; `Modules` rows are
 * `[number, name]`. None (0) and User (1) are implicit.
 */
[[nodiscard]] bool write_gpio_catalog(CommandOutput& output);

/**
 * @brief Writes the selected Module, the user Template, and every topology pin.
 *
 * `Pins` rows are `[pin, slot, flags, capabilities, base, effective]`; slot 255 means the pin
 * has no Template slot, and `base` is the selected template's value (1 = User, editable by
 * `GPIO`).
 */
[[nodiscard]] bool write_gpio_state(CommandOutput& output, const Settings& settings);

} // namespace commands
} // namespace newmota
