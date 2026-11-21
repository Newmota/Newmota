/*
  font_module.h - Berry embedded font class interface

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#pragma once

/** @file font_module.h
 *  @brief Declares installation of the Berry font class.
 */

struct bvm;

namespace newmota {

/** @brief Installs the global Berry \c font class.
 *
 *  @param vm Berry virtual machine receiving the class.
 *  @return True when the class has been registered.
 */
bool install_font_module(bvm* vm);
} // namespace newmota
