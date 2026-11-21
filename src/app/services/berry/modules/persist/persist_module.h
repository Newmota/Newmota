/*
  persist_module.h - Berry persist save interface

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#pragma once
struct bvm;

namespace newmota {
/** @brief Saves the already imported singleton; invoke inside a protected Berry call. */
int save_imported_persist(bvm* vm);
} // namespace newmota
