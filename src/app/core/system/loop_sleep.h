/*
  loop_sleep.h - main-loop sleep state and load average for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file loop_sleep.h
 * @brief Tasmota's SleepMode, Sleep and LoadAvg as reported in STATE and StatusSTS.
 */

#pragma once

#include <cstdint>

namespace newmota::loop_sleep {

struct Status final {
    bool normal{};        /**< SetOption60: fixed wait after each pass instead of dynamic. */
    std::uint8_t sleep{}; /**< Applied Sleep in milliseconds. */
    std::uint32_t load{}; /**< Tasmota's LoadAvg: percent of the Sleep period spent working. */
};

/**
 * @brief Records one main-loop pass with Tasmota's moving average.
 * @param activity_ms Time the pass spent working, before any wait.
 */
void record_pass(std::uint32_t activity_ms, std::uint8_t sleep_ms, bool normal);

[[nodiscard]] Status status();

/** @brief Returns "Dynamic" or "Normal", Tasmota's SleepMode names. */
[[nodiscard]] const char* mode_name(bool normal);

} // namespace newmota::loop_sleep
