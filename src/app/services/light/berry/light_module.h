/*
  light_module.h - Berry binding for the Light owner service

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

struct bvm;

namespace newmota {
class LightService;

/** @brief Binds the live Light owner and exposes the Berry global `light`. */
[[nodiscard]] bool install_light_module(bvm* vm, LightService& light);

/** @brief Clears the non-owning Light owner binding before VM destruction. */
void remove_light_module();

} // namespace newmota
