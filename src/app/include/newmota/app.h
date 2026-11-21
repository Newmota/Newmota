/*
  app.h - application entry point for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

namespace newmota {

/**
 * @brief Runs the firmware application loop.
 *
 * This function initializes core services and does not return.
 */
[[noreturn]] void run();

} // namespace newmota
