/*
  display_service.h - descriptor-driven display startup

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file display_service.h
 * @brief Declares startup for the retained descriptor-driven display owner.
 */

#pragma once

namespace newmota {

class Hardware;

/** @brief Loads `/display.ini`, resolves configured pin wildcards, and initializes once. */
[[nodiscard]] bool initialize_display_from_filesystem(const Hardware&);

} // namespace newmota
