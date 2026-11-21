/*
  energy_module.h - Safe Berry energy module registration

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file energy_module.h
 * @brief Installs value-based energy access without exposing native storage pointers.
 */
#pragma once
struct bvm;

namespace newmota {
/** @brief Registers the energy import and bounded instance/channel proxies.
 * @param vm Application Berry VM with the standard compiler and collection classes.
 * @return true when registration and proxy compilation complete successfully.
 */
bool install_energy_module(bvm* vm);
} // namespace newmota
