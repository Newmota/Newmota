/*
  update_writer.h - Beken native RBL OTA writer declaration

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "newmota/platform/update.h"

namespace newmota::platform::beken {

/** @brief Returns the stock single-image OTA writer for this Beken profile. */
[[nodiscard]] UpdateWriter& native_rbl_update_writer();

} // namespace newmota::platform::beken
