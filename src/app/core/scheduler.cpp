/*
  scheduler.cpp - cooperative scheduler implementation for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "core/scheduler.h"

namespace newmota {

/**
 * @brief Initializes all cadence deadlines relative to @p now.
 */
Scheduler::Scheduler(const std::uint32_t now)
    : next_50_ms_(now + 50), next_100_ms_(now + 100), next_250_ms_(now + 250),
      next_second_(now + 1000) {}

/**
 * @brief Advances every due cadence and returns its tick mask.
 */
ScheduledTick Scheduler::poll(const std::uint32_t now) {
    ScheduledTick ticks = ScheduledTick::none;
    if (advance_if_due(now, 50, next_50_ms_)) {
        ticks = ticks | ScheduledTick::every_50_ms;
    }
    if (advance_if_due(now, 100, next_100_ms_)) {
        ticks = ticks | ScheduledTick::every_100_ms;
    }
    if (advance_if_due(now, 250, next_250_ms_)) {
        ticks = ticks | ScheduledTick::every_250_ms;
    }
    if (advance_if_due(now, 1000, next_second_)) {
        ticks = ticks | ScheduledTick::every_second;
    }
    return ticks;
}

/**
 * @brief Advances one rollover-safe deadline when it has become due.
 * @return true when the deadline was due.
 */
bool Scheduler::advance_if_due(
    const std::uint32_t now, const std::uint32_t period, std::uint32_t& deadline) {
    constexpr std::uint32_t kHalfTimerRange = 1U << 31;
    const std::uint32_t lateness = now - deadline;
    if (lateness >= kHalfTimerRange) {
        return false;
    }

    const std::uint32_t elapsed_periods = lateness / period;
    deadline += (elapsed_periods + 1U) * period;
    return true;
}

} // namespace newmota
