/*
  deep_sleep.h - portable deep-sleep entry and retained wake state for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file deep_sleep.h
 * @brief Timed deep sleep that ends in a fresh boot, plus the few words that survive it.
 *
 * A wake reports RestartReason::deep_sleep. Platforms without memory that survives deep sleep
 * report no retained state; DeepSleepTime then plans every cycle from the clock alone.
 */

#pragma once

#include <cstdint>

namespace newmota::platform::deep_sleep {

/** @brief Tasmota's RtcSettings deep-sleep fields. */
struct Retained final {
    std::uint32_t next_wakeup{}; /**< Planned local wake time in seconds. */
    std::uint32_t slip{};        /**< Timer correction in 1/10000; 10000 is exact. */
    std::uint32_t remaining{};   /**< Seconds left of a sleep longer than one cycle. */
};

/** @brief Reads the record kept across the last deep sleep.
 * @return false where nothing survives deep sleep or the record fails its check.
 */
[[nodiscard]] bool load(Retained& state);

/** @brief Keeps @p state for the next wake; does nothing where nothing survives deep sleep. */
void store(const Retained& state);

/** @brief Reports whether load() and store() keep state across deep sleep. */
[[nodiscard]] bool retained();

/** @brief Longest single timed sleep the hardware supports, in seconds. */
[[nodiscard]] std::uint32_t maximum_seconds();

/**
 * @brief Powers down until the timer or the ULP coprocessor wakes the chip, then boots afresh.
 * @param microseconds Timer wake delay; 0 sets no timer.
 * @param coprocessor_wake Also wake when the ULP coprocessor asks; ignored without one.
 */
[[noreturn]] void enter(std::uint64_t microseconds, bool coprocessor_wake);

} // namespace newmota::platform::deep_sleep
