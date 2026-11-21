/*
  rtc_chips.h - Optional RTC protocol descriptors

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file rtc_chips.h
 * @brief Independently selected calendar protocols; no probe can identify these chip models.
 */
#pragma once
#include "drivers/rtc/rtc_driver.h"

namespace newmota::rtc {
extern const Driver ds1307_driver;
extern const Driver bm8563_driver;
extern const Driver pcf85063_driver;
extern const Driver pcf85363_driver;
extern const Driver rv3028_driver;
extern const Driver rx8010_driver;
extern const Driver rx8025_driver;
extern const Driver rx8130_driver;
} // namespace newmota::rtc
