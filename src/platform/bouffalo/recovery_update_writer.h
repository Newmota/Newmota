/*
  recovery_update_writer.h - Bouffalo Safeboot OTA writer declaration

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file recovery_update_writer.h
 * @brief Bouffalo Safeboot OTA writer declaration.
 */

#pragma once

#include "newmota/platform/update.h"

namespace newmota::platform {

[[nodiscard]] UpdateWriter& recovery_update_writer();

} // namespace newmota::platform
