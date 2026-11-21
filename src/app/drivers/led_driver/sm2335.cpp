/*
  sm2335.cpp - SM2335 five-channel LED driver frames

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Frame reference: Tasmota xlgt_09_sm2335.ino, Copyright (C) 2021 Theo Arends and Cossid;
  separate RGB and white current nibbles from OpenBeken drv_sm2235.c; GPL version 3 or later.
*/

/** @file sm2335.cpp
 * @brief SM2335 grayscale and standby frames; OpenBeken drives SM2235 and SM2185N the same way.
 */

#include "drivers/led_driver/led_driver.h"

namespace newmota::led_driver::chips {
namespace {
// 11 = identification, 0 = reserved, 11 = all five channels (or 00 = standby), 000 = OUT1.
constexpr std::uint8_t kFiveChannels = 0xD8U;
constexpr std::uint8_t kStandby = 0xC0U;
} // namespace

void sm2335_write(Bus& bus, const std::array<std::uint16_t, 5>& values,
    const std::array<std::uint8_t, 2>& currents) {
    // Values go as high byte, then low byte.
    nibble_current_write(bus, kFiveChannels, kStandby, false, values, currents);
}

} // namespace newmota::led_driver::chips
