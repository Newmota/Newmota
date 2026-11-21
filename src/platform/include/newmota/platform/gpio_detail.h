/*
  gpio_detail.h - internal topology validation for Newmota GPIO adapters

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "newmota/platform/gpio.h"

#include <cstdint>

namespace newmota::platform::detail {

/** @brief Validates pin availability and every bit in @p requirements. */
[[nodiscard]] GpioResult validate_gpio(PinId pin, std::uint16_t requirements);
/** @brief Makes console pins available after the platform stopped its console. */
void release_console_pins();

} // namespace newmota::platform::detail
