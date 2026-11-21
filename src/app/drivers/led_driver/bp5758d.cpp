/*
  bp5758d.cpp - BP5758D five-channel LED driver frames

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Frame reference: Tasmota xlgt_08_bp5758d.ino, Copyright (C) 2022 Theo Arends and oomg;
  per-output currents in mA from OpenBeken drv_bp5758d.c; GPL version 3 or later.
*/

/** @file bp5758d.cpp
 * @brief BP5758D setup, grayscale and sleep frames.
 */

#include "drivers/led_driver/led_driver.h"

namespace newmota::led_driver::chips {
namespace {
// Command byte: 10 = identification, 01 = outputs on, low bits select the first register.
constexpr std::uint8_t kSleep = 0x80U;
constexpr std::uint8_t kSetup = 0x90U;
constexpr std::uint8_t kGrayscale = 0x96U;
constexpr std::uint8_t kOutputsOn = 0x1FU;
constexpr std::uint8_t kOutputsOff = 0x00U;

/** @brief Frame position sent to OUT1..OUT5, Tasmota's order: OUT4 takes position 4. */
constexpr std::array<std::uint8_t, 5> kOutputPosition{{0U, 1U, 2U, 4U, 3U}};

/** @brief Converts mA to the current code: bits 0..5 weigh 1..32 mA, bit 6 30 mA. */
constexpr std::uint8_t current_code(const std::uint8_t milliamps) {
    return milliamps > 63U ? static_cast<std::uint8_t>(milliamps + 34U) : milliamps;
}
} // namespace

void bp5758d_setup(Bus& bus, const std::array<std::uint8_t, 5>& channels,
    const std::array<std::uint8_t, 2>& currents, const bool clear) {
    bus.start(kSetup);
    bus.write(kOutputsOn);
    // An output carrying red, green or blue takes the RGB current, white the white current.
    for (const auto position : kOutputPosition)
        bus.write(current_code(currents[channels[position] < 3U ? 0U : 1U]));
    if (clear)
        for (std::size_t i = 0U; i < 10U; ++i)
            bus.write(0U);
    bus.stop();
}

void bp5758d_write(Bus& bus, const std::array<std::uint16_t, 5>& values, bool& sleeping) {
    bool dark = true;
    for (const auto value : values)
        dark = dark && ten_bits(value) == 0U;
    if (dark) {
        bus.start(kSetup);
        bus.write(kOutputsOff);
        bus.stop();
        bus.start(kSleep);
        bus.stop();
        sleeping = true;
        return;
    }
    if (sleeping) {
        // Setting the outputs again also leaves sleep.
        bus.start(kSetup);
        bus.write(kOutputsOn);
        bus.stop();
        sleeping = false;
    }
    bus.start(kGrayscale);
    for (const auto position : kOutputPosition) {
        const auto value = ten_bits(values[position]);
        bus.write(static_cast<std::uint8_t>(value & 0x1FU));
        bus.write(static_cast<std::uint8_t>(value >> 5U));
    }
    bus.stop();
}

} // namespace newmota::led_driver::chips
