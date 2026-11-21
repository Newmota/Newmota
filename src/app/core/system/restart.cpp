/*
  restart.cpp - restart controller implementation for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file restart.cpp
 * @brief Restart scheduling and final application save callbacks.
 */

#include "core/system/restart.h"

#include "newmota/log.h"
#if NEWMOTA_DEEP_SLEEP
#include "newmota/platform/deep_sleep.h"
#endif
#include "newmota/platform/platform.h"

#include <cstdint>

namespace newmota {
/**
 * @brief Schedules a delayed restart and prepares an alternate boot if requested.
 */
RestartRequestResult RestartController::schedule(
    const RestartTarget target, const std::uint32_t now) {
    if (pending_) {
        return RestartRequestResult::busy;
    }

    if (target != RestartTarget::current) {
        switch (platform::prepare_boot_partition_toggle(
            target == RestartTarget::alternate ? platform::AlternateImage::paired
                                               : platform::AlternateImage::any_recovery)) {
        case platform::BootPartitionSwitchResult::prepared:
            break;
        case platform::BootPartitionSwitchResult::unavailable:
            return RestartRequestResult::unavailable;
        case platform::BootPartitionSwitchResult::failed:
            return RestartRequestResult::failed;
        }
    }

    deadline_ = now + kDelayMilliseconds;
    pending_ = true;
    return RestartRequestResult::scheduled;
}

#if NEWMOTA_DEEP_SLEEP
/**
 * @brief Accepts deep sleep as a restart that powers down instead of booting at once.
 */
RestartRequestResult RestartController::schedule_deep_sleep(
    const std::uint64_t microseconds, const std::uint32_t now) {
    if (pending_) {
        return RestartRequestResult::busy;
    }
    sleep_microseconds_ = microseconds;
    sleep_ = true;
    deadline_ = now + kDelayMilliseconds;
    pending_ = true;
    return RestartRequestResult::scheduled;
}

/**
 * @brief Closes the network sessions and hands over to the platform deep sleep.
 */
[[noreturn]] void RestartController::deep_sleep_now(
    const std::uint64_t microseconds, const bool coprocessor_wake) {
    if (network_shutdown_callback_ != nullptr) {
        network_shutdown_callback_(network_shutdown_context_);
    }
    AddLog(LogLevel::info, "APP: Deep sleep");
    platform::deep_sleep::enter(microseconds, coprocessor_wake);
}
#endif

/**
 * @brief Logs and immediately invokes the platform restart operation.
 */
[[noreturn]] void RestartController::restart_now() {
    if (before_restart_callback_ != nullptr) {
        before_restart_callback_(before_restart_context_);
    }
    AddLog(LogLevel::info, "APP: Restarting");
    platform::restart();
}

/**
 * @brief Performs a scheduled restart after the response delay expires.
 */
void RestartController::poll(const std::uint32_t now) {
    if (!pending_ || static_cast<std::int32_t>(now - deadline_) < 0) {
        return;
    }
    pending_ = false;
#if NEWMOTA_DEEP_SLEEP
    if (sleep_) {
        if (before_restart_callback_ != nullptr) {
            before_restart_callback_(before_restart_context_);
        }
        deep_sleep_now(sleep_microseconds_, false);
    }
#endif
    restart_now();
}

} // namespace newmota
