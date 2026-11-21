/*
  global_temperature_selection.h - Typed configured global-temperature collection

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  GetSensorValues reference: Tasmota support_tasmota.ino,
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
*/

/** @file global_temperature_selection.h
 * @brief Collects a bounded typed field ordinal without parsing serialized JSON substrings.
 */
#pragma once
#include <cstdint>

namespace newmota {
class Settings;
/** @brief Refreshes nonzero source selection at a ten-second or actual SENSOR boundary.
 * @param settings Read-only presentation settings; configured ordinal is held by the owner.
 * @param now Monotonic application milliseconds.
 * @param publication true when preparing a real SENSOR document, not a status/period commit.
 * Missing fields retain the old value; an absent ordinal falls back to the first field.
 * This does not poll hardware, write settings or modify physical sensor/energy samples.
 */
void refresh_global_temperature_selection(
    const Settings& settings, std::uint32_t now, bool publication = false);
} // namespace newmota
