/*
  display_module.h - Berry display primitive class

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file display_module.h
 * @brief Declares the retained Berry display primitive class.
 */

#pragma once

struct bvm;

namespace newmota {

/** @brief Installs the global Berry `display` class over the retained panel. */
[[nodiscard]] bool install_display_module(bvm* vm);

} // namespace newmota
