/*
  boot_guard.cpp - quick-restart tracking for the boot-loop fallback in Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "core/system/boot_guard.h"

namespace newmota {
namespace {

constexpr std::uint32_t kStableBootMilliseconds = 10000U; // Tasmota BOOT_LOOP_TIME
constexpr std::uint32_t kHalfTimerRange = 1U << 31;

/** @brief Tests a wrapping millisecond deadline. */
[[nodiscard]] bool DeadlineReached(const std::uint32_t now, const std::uint32_t deadline) {
    return now - deadline < kHalfTimerRange;
}

BootGuard guard;

} // namespace

BootGuard& boot_guard() {
    return guard;
}

/** @brief Counts this boot before any startup work that could crash. */
void BootGuard::begin(const std::uint32_t now) {
    restart_ = platform::begin_fast_restart_tracking();
    stable_deadline_ = now + kStableBootMilliseconds;
    stable_ = false;
}

/** @brief Ends quick-restart tracking after the stable window. */
void BootGuard::poll(const std::uint32_t now) {
    if (stable_ || !DeadlineReached(now, stable_deadline_))
        return;
    // Also confirms the Bouffalo FW entry and a pending RP2350 trial image, so it runs even
    // where the count is not retained.
    clear();
}

/** @brief Ends quick-restart tracking at once. */
void BootGuard::clear() {
    // An unbought RP2350 trial image reverts on the first quick restart, so a boot loop that
    // reaches Safeboot never confirms one here.
    platform::mark_boot_stable();
    stable_ = true;
}

} // namespace newmota
