/*
  deep_sleep_wake.h - Pico deep-sleep wake detection

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

namespace newmota::platform::pico_detail {

/** @brief Reports whether this boot is the wake from deep_sleep::enter(). */
[[nodiscard]] bool woke_from_deep_sleep();

} // namespace newmota::platform::pico_detail
