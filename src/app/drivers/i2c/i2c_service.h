/*
  i2c_service.h - owned I2C and optional driver support for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once
#include "core/hardware/hardware.h"

namespace newmota {
/** @brief Initializes resolved SCL/SDA bus pairs at startup. */
void initialize_i2c(const Hardware& hardware);
} // namespace newmota
