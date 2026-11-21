/*
  sm2135.cpp - SM2135 five-channel LED driver frames

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Frame and preset reference: Tasmota xlgt_04_sm2135.ino, Copyright (C) 2021 Theo Arends and
  CrudelisPL, GPL version 3 or later.
*/

/** @file sm2135.cpp
 * @brief SM2135 RGB and white frames for Tasmota's ten `SM2135 DAT` presets.
 */

#include "drivers/led_driver/led_driver.h"
#include "newmota/platform/platform.h"

namespace newmota::led_driver::chips {
namespace {
constexpr std::uint8_t kCurrent = 0xC0U; // Max current register, followed by the mode.
constexpr std::uint8_t kCold = 0xC5U;    // White levels from the cold register on.
constexpr std::uint8_t kRgbMode = 0x00U;
constexpr std::uint8_t kWhiteMode = 0x80U;

/** @brief One Tasmota preset; codes 0..10 step 5 mA from 10 mA. */
struct Preset final {
    std::uint8_t rgb_current;
    std::uint8_t white_current;
    bool bgr;       /**< Blue, green, red instead of green, red, blue. */
    bool exclusive; /**< RGB or white, never both: the bulb's supply cannot feed both. */
    bool clear;     /**< Zero the group that is not sent. */
    bool set_all;   /**< One RGB-mode frame with all five values (Fitop). */
};

// DAT1..DAT10 in Tasmota's order (Sm2135Color). DAT8 is reserved there and runs its default
// code path: BGR, the default currents, RGB and white together.
constexpr std::array<Preset, 10> kPresets{{
    {2U, 1U, false, true, false, false},   // GRB, 20/15 mA (Action LSC)
    {2U, 1U, true, true, false, false},    // BGR, 20/15 mA (Polux)
    {2U, 4U, false, false, false, false},  // GRB, 20/30 mA
    {2U, 4U, true, false, false, false},   // BGR, 20/30 mA
    {7U, 10U, false, false, false, false}, // GRB, 45/60 mA (LE LampUX)
    {7U, 10U, true, false, false, false},  // BGR, 45/60 mA
    {1U, 6U, false, false, false, true},   // BRG, 15/40 mA, all at once (Fitop)
    {2U, 1U, true, false, false, false},   // reserved
    {2U, 1U, false, true, true, false},    // GRB, 20/15 mA, clears the other group (Qualitel)
    {2U, 1U, true, true, true, false},     // BGR, 20/15 mA, clears the other group
}};

const Preset& preset_of(const std::uint8_t preset) {
    return kPresets[preset >= 1U && preset <= kPresets.size() ? preset - 1U : 0U];
}

/** @brief Scales a 16-bit electrical value to the chip's 8 bits. */
constexpr std::uint8_t eight_bits(const std::uint16_t value) {
    return static_cast<std::uint8_t>((static_cast<std::uint32_t>(value) * 255U + 32767U) / 65535U);
}

void send_rgb(Bus& bus, const std::uint8_t current, const bool bgr, const std::uint8_t red,
    const std::uint8_t green, const std::uint8_t blue) {
    bus.start(kCurrent);
    bus.write(current);
    bus.write(kRgbMode);
    bus.write(bgr ? blue : green);
    bus.write(bgr ? green : red);
    bus.write(bgr ? red : blue);
    bus.stop();
}

void send_white(
    Bus& bus, const std::uint8_t current, const std::uint8_t cold, const std::uint8_t warm) {
    bus.start(kCurrent);
    bus.write(current);
    bus.write(kWhiteMode);
    bus.stop();
    platform::delay_microseconds(1000U); // Tasmota waits 1 ms before the white levels.
    bus.start(kCold);
    // Tasmota's order: the fifth Light channel first.
    bus.write(warm);
    bus.write(cold);
    bus.stop();
}
} // namespace

std::array<std::uint8_t, 2> sm2135_currents(const std::uint8_t preset) {
    const auto& chosen = preset_of(preset);
    return {{chosen.rgb_current, chosen.white_current}};
}

void sm2135_write(Bus& bus, const std::array<std::uint16_t, 5>& values,
    const std::array<std::uint8_t, 2>& currents, const std::uint8_t preset) {
    const auto& chosen = preset_of(preset);
    std::array<std::uint8_t, 5> level{};
    for (std::size_t i = 0U; i < level.size(); ++i)
        level[i] = eight_bits(values[i]);
    const auto current = static_cast<std::uint8_t>((currents[0] << 4U) | currents[1]);
    if (chosen.set_all) {
        // The Fitop bulb keeps a group lit unless every frame carries all five values.
        bus.start(kCurrent);
        bus.write(current);
        bus.write(kRgbMode);
        bus.write(level[2]);
        bus.write(level[0]);
        bus.write(level[1]);
        bus.write(level[4]);
        bus.write(level[3]);
        bus.stop();
        return;
    }
    bool rgb = true;
    bool white = true;
    if (chosen.exclusive) {
        white = level[3] != 0U || level[4] != 0U;
        rgb = !white;
    }
    if (rgb)
        send_rgb(bus, current, chosen.bgr, level[0], level[1], level[2]);
    else if (chosen.clear)
        send_rgb(bus, current, chosen.bgr, 0U, 0U, 0U);
    if (white)
        send_white(bus, current, level[3], level[4]);
    else if (chosen.clear)
        send_white(bus, current, 0U, 0U);
}

} // namespace newmota::led_driver::chips
