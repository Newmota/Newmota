/*
  serial_module.h - Berry serial module lifecycle interface

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#pragma once
struct bvm;

namespace newmota {
/** @brief Registers the global class and all 24 upstream framing constants. */
bool install_serial_module(bvm*);
/** @brief Closes retained VM handles while preserving uncertain native ownership. */
void remove_serial_module();
} // namespace newmota
