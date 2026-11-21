/*
  wire_module.h - owned I2C and optional driver support for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file wire_module.h
 * @brief Owned I2C and optional driver support for Newmota.
 */

/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
struct bvm;

namespace newmota {
/** @brief Registers the Berry Wire class and optional I2C driver modules. */
bool install_wire_module(bvm* vm);
/** @brief Removes Wire bindings and releases Berry-side module state. */
void remove_wire_module();
} // namespace newmota
