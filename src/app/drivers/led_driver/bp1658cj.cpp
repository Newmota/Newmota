/*
  bp1658cj.cpp - BP1658CJ five-channel LED driver frames

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Frame reference: Tasmota xlgt_10_bp1658cj.ino, Copyright (C) 2022 Theo Arends and Cossid;
  separate RGB and white current nibbles from OpenBeken drv_bp1658cj.c; GPL version 3 or later.
*/

/** @file bp1658cj.cpp
 * @brief BP1658CJ grayscale and standby frames.
 */

#include "drivers/led_driver/led_driver.h"

namespace newmota::led_driver::chips {
namespace {
// 10 = identification, 11 = all five channels (or 00 = standby), 0000 = start at OUT1.
constexpr std::uint8_t kFiveChannels = 0xB0U;
constexpr std::uint8_t kStandby = 0x80U;
} // namespace

void bp1658cj_write(Bus& bus, const std::array<std::uint16_t, 5>& values,
    const std::array<std::uint8_t, 2>& currents) {
    // Values go as low 5 bits, then high 5 bits.
    nibble_current_write(bus, kFiveChannels, kStandby, true, values, currents);
}

} // namespace newmota::led_driver::chips
