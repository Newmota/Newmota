/*
  led_strip_module.h - Berry LED strip module lifecycle interface

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file led_strip_module.h
 * @brief Berry LED strip module lifecycle interface.
 */

#pragma once
struct bvm;

namespace newmota {
/** @brief Registers the Berry LED strip class and native methods. */
bool install_led_strip_module(bvm*);
/** @brief Removes Berry LED strip bindings before VM teardown. */
void remove_led_strip_module();
} // namespace newmota
