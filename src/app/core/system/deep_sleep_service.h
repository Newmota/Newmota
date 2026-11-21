/*
  deep_sleep_service.h - Tasmota's DeepSleepTime for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file deep_sleep_service.h
 * @brief Ported from Tasmota's xdrv_29_deepsleep.ino.
 *
 * After each TelePeriod the device counts down four seconds (64 after a boot that was not a
 * deep-sleep wake) and sleeps until the next multiple of DeepSleepTime, correcting the timer
 * slip between cycles. A low GPIO_DEEPSLEEP pin keeps it awake. The "Wakeup" rule is absent
 * because Newmota has no Rules. The value lives in `/.newmota/deepsleep.bin`.
 */

#pragma once

#include "core/command/command.h"
#include "core/hardware/hardware.h"

#include <cstdint>
#include <string_view>

namespace newmota {
class RestartController;
class Settings;
class TimeService;
} // namespace newmota

namespace newmota::deep_sleep {

/** @brief Publishes the retained tele/DeepSleep payload; false when it could not be queued. */
using Publisher = bool (*)(void* context, std::string_view payload);

/**
 * @brief Loads DeepSleepTime and, on a wake with sleep left over, goes straight back to sleep.
 *
 * Call after the hardware is resolved and before the network starts.
 */
void initialize(const Hardware& hardware);

/** @brief Tasmota's DeepSleepEnabled: a valid time and no GPIO_DEEPSLEEP pin held low. */
[[nodiscard]] bool enabled();

/** @brief Installs the tele/DeepSleep publisher; null leaves the event unpublished. */
void bind_publisher(Publisher publisher, void* context);

/**
 * @brief Runs the TelePeriod hook, the network timeout and the countdown once per second.
 * @param network_up Whether the device has a network address.
 * @param now Monotonic milliseconds.
 */
void poll_second(Settings& settings, const TimeService& time, RestartController& restart,
    bool network_up, std::uint32_t now);

/** @brief Handles DeepSleepTime; unknown for anything else. */
[[nodiscard]] CommandStatus command(
    const CommandRequest& request, Settings& settings, CommandOutput& output);

} // namespace newmota::deep_sleep
