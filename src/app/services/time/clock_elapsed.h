/*
  clock_elapsed.h - bounded stale-caller handling for the shared UTC clock

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file clock_elapsed.h
 * @brief Prevents cached loop timestamps from turning a new UTC anchor into a 49-day jump.
 */
#pragma once
#include <cstdint>

namespace newmota::time_clock {
/** @brief Computes wrapping forward elapsed time, clamping bounded stale callers to zero.
 * A command or clock poll can accept/rebase a sample after a consumer cached its
 * loop timestamp. Such callers may lag the anchor by at most 60 seconds. The
 * clock owner polls regularly (normally 50 ms); a single forward gap within 60
 * seconds of a complete uint32 millisecond cycle is outside this contract.
 */
[[nodiscard]] constexpr std::uint32_t elapsed(std::uint32_t now, std::uint32_t anchor) {
    const auto difference = static_cast<std::uint32_t>(now - anchor);
    return difference >= UINT32_MAX - 59'999U ? 0 : difference;
}
} // namespace newmota::time_clock
