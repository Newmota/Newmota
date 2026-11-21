/*
  gpio_module.h - Berry GPIO module lifecycle interface

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file gpio_module.h
 * @brief Berry GPIO module lifecycle interface.
 */

#pragma once
struct bvm;

namespace newmota {
class Hardware;
/** @brief Installs Berry GPIO bindings backed by the resolved hardware model. */
void install_gpio_module(const Hardware* hardware);
/** @brief Removes Berry GPIO bindings before the VM is destroyed. */
void remove_gpio_module();
} // namespace newmota
