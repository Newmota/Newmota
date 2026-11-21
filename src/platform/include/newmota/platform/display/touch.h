/*
  touch.h - bounded universal I2C touch contract

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file touch.h
 * @brief Descriptor touch programs and cached cooperative pointer samples.
 */
#pragma once
#include "newmota/platform/display.h"
#include <array>
#include <string_view>

namespace newmota::platform::display {
/** @brief Current touch availability, last transfer health and logical pointer position. */
struct TouchState {
    bool available{}, healthy{}, pressed{};
    std::uint16_t x{}, y{};
};

/** @brief Reads the cached sample without I2C operations; application-task use only. */
TouchState touch_state();

namespace detail {
/** @brief Fixed bytecode storage; compilation reserves one byte for implicit termination. */
struct TouchProgram {
    std::array<std::uint8_t, 64> code{};
    std::size_t size{};
    bool present{};
};

/** @brief I2C controller configuration retained with the display descriptor. */
struct TouchConfig {
    char name[16]{};
    int bus{}, address{}, reset{-1}, irq{-1};
    bool configured{}, calibrated{};
    std::array<int, 4> calibration{};
    std::array<std::uint8_t, 4> transforms{{0, 1, 2, 3}};
    std::array<TouchProgram, 4> programs{}; // Init, detection, X and Y.
};

/** @brief Parses an exact UTI I1/I2 header without touching hardware. */
bool touch_header(std::string_view, TouchConfig&);
/** @brief Appends checked instructions from one descriptor continuation line.
 * @return false for unsupported, malformed or oversized instructions; the caller
 * must discard the partially compiled configuration on failure.
 */
bool touch_compile(std::string_view, TouchProgram&);
/** @brief Resets and identifies the declared I2C controller after pins/address are reserved. */
Result touch_open(const TouchConfig&, std::uint16_t width, std::uint16_t height);
/** @brief Samples cooperatively, releasing the pointer on any incomplete/failed transaction. */
void touch_poll(std::uint8_t rotation, std::uint16_t width, std::uint16_t height);
/** @brief Invalidates a cached press immediately when display orientation changes. */
void touch_release();
} // namespace detail
} // namespace newmota::platform::display
