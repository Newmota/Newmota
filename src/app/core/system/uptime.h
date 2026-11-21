/*
  uptime.h - boot uptime second counter for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstdint>

namespace newmota {

/**
 * @brief Counts whole seconds since boot past the 49.7-day millisecond clock wrap.
 *
 * Like Tasmota's uptime, this is a 32-bit second counter, but it adds the seconds elapsed since
 * the last call instead of one per tick, so a stalled main loop loses none. Call it on the
 * application task only, and at least once per clock wrap; the main loop does so every second.
 *
 * @return Seconds since the millisecond clock started.
 */
std::uint32_t uptime_seconds();

} // namespace newmota
