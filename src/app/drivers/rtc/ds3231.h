/*
  ds3231.h - DS3231 UTC calendar protocol

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file ds3231.h
 * @brief Opt-in DS3231 at 0x68; the address does not uniquely identify this device.
 */
#pragma once
#include "drivers/rtc/rtc_driver.h"

namespace newmota::rtc {
extern const Driver ds3231_driver;
} // namespace newmota::rtc
