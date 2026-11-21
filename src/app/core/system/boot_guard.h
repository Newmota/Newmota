/*
  boot_guard.h - quick-restart tracking for the boot-loop fallback in Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstdint>

#include "newmota/platform/platform.h"

namespace newmota {

/** @brief Quick restarts from which this boot skips BLE, Matter, LVGL autostart and autoexec. */
inline constexpr std::uint8_t kBootLoopReducedCount = 4U;
/** @brief Quick restarts from which this boot also starts without the user GPIO overrides. */
inline constexpr std::uint8_t kBootLoopNoGpioCount = 5U;
/** @brief Quick restarts from which the application restarts into the paired Safeboot. */
inline constexpr std::uint8_t kBootLoopSafebootCount = 10U;

/**
 * @brief Counts consecutive quick application restarts in retained platform state.
 *
 * Tasmota's fast-reboot counter: begin() runs first in the application, so a crash anywhere
 * in startup counts, and ten seconds of runtime clear the count. Consumers skip features for
 * this boot only from the count; nothing here writes settings.
 */
class BootGuard final {
  public:
    /**
     * @brief Increments the retained count for this boot and starts the stable window.
     * @param now Current wrapping monotonic milliseconds.
     */
    void begin(std::uint32_t now);

    /** @brief Clears the retained count once the boot has run for the stable window. */
    void poll(std::uint32_t now);

    /** @brief Clears the retained count now, before a deliberate restart into Safeboot. */
    void clear();

    /** @brief Returns this boot's count, 1 for a normal start, and whether it is retained. */
    [[nodiscard]] platform::FastRestartInfo restart() const { return restart_; }

    /** @brief Reports a retained count that reached @p threshold quick restarts. */
    [[nodiscard]] bool reached(const std::uint8_t threshold) const {
        return restart_.available && restart_.count >= threshold;
    }

    /** @brief Reports a retained count that reached kBootLoopReducedCount. */
    [[nodiscard]] bool reduced() const { return reached(kBootLoopReducedCount); }

  private:
    std::uint32_t stable_deadline_{};
    platform::FastRestartInfo restart_{};
    bool stable_{};
};

/** @brief Returns the application's boot guard; never begun in Safeboot. */
BootGuard& boot_guard();

} // namespace newmota
