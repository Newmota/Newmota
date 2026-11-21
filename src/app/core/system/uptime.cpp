/*
  uptime.cpp - boot uptime second counter for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "core/system/uptime.h"

#include "newmota/platform/platform.h"

namespace newmota {

std::uint32_t uptime_seconds() {
    static std::uint32_t seconds{}, counted_ms{};
    const std::uint32_t whole = (platform::monotonic_milliseconds() - counted_ms) / 1000U;
    seconds += whole;
    counted_ms += whole * 1000U;
    return seconds;
}

} // namespace newmota
