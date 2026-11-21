/*
  restart.h - restart controller interface for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file restart.h
 * @brief Restart scheduling and final application save callbacks.
 */

#pragma once

#include "newmota_features.h"

#include <cstdint>

/** @brief Deep sleep is linked when DeepSleepTime or ULP.sleep can request it. */
#define NEWMOTA_DEEP_SLEEP (USE_DEEPSLEEP || USE_BERRY_ULP)

namespace newmota {

enum class RestartTarget : std::uint8_t {
    current,
    /** Paired Newmota recovery image only (automatic update handoff). */
    alternate,
    /** Operator-requested recovery; may enter a verified foreign recovery image. */
    alternate_any,
};

enum class RestartRequestResult : std::uint8_t {
    scheduled,
    unavailable,
    failed,
    busy,
};

class RestartController final {
  public:
    using BeforeRestartCallback = void (*)(void* context);
    using NetworkShutdownCallback = void (*)(void* context);

    /** @brief Installs the optional final synchronous save hook. */
    void set_before_restart_callback(BeforeRestartCallback callback, void* context) {
        before_restart_callback_ = callback;
        before_restart_context_ = context;
    }

#if NEWMOTA_DEEP_SLEEP
    /** @brief Installs the hook that closes network sessions before deep sleep. */
    void set_network_shutdown_callback(NetworkShutdownCallback callback, void* context) {
        network_shutdown_callback_ = callback;
        network_shutdown_context_ = context;
    }
#endif

    /**
     * @brief Schedules a delayed restart, optionally selecting the alternate image.
     * @return The scheduling result.
     */
    [[nodiscard]] RestartRequestResult schedule(RestartTarget target, std::uint32_t now);
    /**
     * @brief Saves application state and immediately restarts without returning.
     */
    [[noreturn]] void restart_now();

#if NEWMOTA_DEEP_SLEEP
    /**
     * @brief Schedules deep sleep after the response delay, saving state as a restart does.
     * @param microseconds Timer wake delay; 0 sets no timer.
     */
    [[nodiscard]] RestartRequestResult schedule_deep_sleep(
        std::uint64_t microseconds, std::uint32_t now);
    /**
     * @brief Closes network sessions and enters deep sleep at once, without the final save.
     * @param microseconds Timer wake delay; 0 sets no timer.
     * @param coprocessor_wake Also wake on a ULP request.
     */
    [[noreturn]] void deep_sleep_now(std::uint64_t microseconds, bool coprocessor_wake);
#endif

    /** @brief Reports whether a delayed restart has already been accepted. */
    [[nodiscard]] bool pending() const { return pending_; }

    /**
     * @brief Performs a pending restart after its response delay expires.
     */
    void poll(std::uint32_t now);

  private:
    static constexpr std::uint32_t kDelayMilliseconds = 1000;

    BeforeRestartCallback before_restart_callback_{nullptr};
    void* before_restart_context_{nullptr};
#if NEWMOTA_DEEP_SLEEP
    NetworkShutdownCallback network_shutdown_callback_{nullptr};
    void* network_shutdown_context_{nullptr};
    std::uint64_t sleep_microseconds_{0};
    bool sleep_{false};
#endif
    std::uint32_t deadline_{0};
    bool pending_{false};
};

} // namespace newmota
