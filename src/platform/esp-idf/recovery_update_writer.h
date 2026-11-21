/*
  recovery_update_writer.h - ESP-IDF Safeboot application writer declaration

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "newmota/platform/update.h"

namespace newmota::platform {

/** @brief Returns the ESP-IDF Safeboot-only native app0 writer. */
[[nodiscard]] UpdateWriter& recovery_update_writer();

} // namespace newmota::platform

namespace newmota::platform::esp_idf {

/** @brief App updates are safe only while recovery runs and remains selected. */
[[nodiscard]] constexpr bool recovery_partition_is_selected(
    const bool recovery_is_running, const bool recovery_is_selected) noexcept {
    return recovery_is_running && recovery_is_selected;
}

} // namespace newmota::platform::esp_idf
