/*
  reset_commands.cpp - reset command implementation for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "core/system/reset_commands.h"

#include "core/system/boot_counter.h"
#include "core/system/restart.h"
#include "newmota/platform/platform.h"
#include "services/settings/settings.h"
#include "support/number.h"
#include "support/text.h"

#include <cstdint>

namespace newmota::commands {
namespace {

/** @brief Writes one Reset JSON response with the requested command status. */
[[nodiscard]] CommandStatus write_result(
    CommandOutput& output, const char* result, const CommandStatus status) {
    return output.format("{\"Reset\":\"%s\"}", result) ? status : CommandStatus::failed;
}

/** @brief Schedules the ordinary reboot which completes a successful reset. */
[[nodiscard]] CommandStatus schedule_restart(
    RestartController& restart, CommandOutput& output, const bool erased) {
    if (restart.schedule(RestartTarget::current, platform::monotonic_milliseconds()) !=
        RestartRequestResult::scheduled) {
        return write_result(output, "Restart pending", CommandStatus::failed);
    }
    return write_result(output, erased ? "Erase, Reset and Restarting" : "Reset and Restarting",
        CommandStatus::handled);
}

/** @brief Applies a default-settings reset and schedules its reboot. */
[[nodiscard]] CommandStatus reset_settings(Settings& settings, RestartController& restart,
    CommandOutput& output, const bool preserve_wifi, const bool erase_first) {
    if (settings.reset_to_defaults(preserve_wifi, erase_first) != SettingsUpdateResult::saved) {
        return write_result(output, "Settings reset failed", CommandStatus::failed);
    }
    return schedule_restart(restart, output, erase_first);
}

/** @brief Erases native radio calibration and schedules its reboot. */
[[nodiscard]] CommandStatus reset_calibration(RestartController& restart, CommandOutput& output) {
    switch (platform::erase_radio_calibration()) {
    case platform::RadioCalibrationResetResult::erased:
        return schedule_restart(restart, output, true);
    case platform::RadioCalibrationResetResult::unavailable:
        return write_result(output, "Calibration reset unsupported", CommandStatus::failed);
    case platform::RadioCalibrationResetResult::failed:
        return write_result(output, "Calibration reset failed", CommandStatus::failed);
    }
    return CommandStatus::failed;
}

} // namespace

/** @brief Dispatches Reset independently from the Restart command family. */
CommandStatus dispatch_reset(const CommandRequest& request, Settings& settings,
    RestartController& restart, BootCounter* const boot_counter, CommandOutput& output) {
    if (!support::ascii_iequals(request.name, "reset")) {
        return CommandStatus::unknown;
    }

    std::uint32_t mode = 0;
    if (request.has_index || request.is_query() ||
        !support::parse_unsigned_decimal(request.payload, 99, mode)) {
        return write_result(output, "1 to reset", CommandStatus::handled);
    }
    if (restart.pending()) {
        return write_result(output, "Restart pending", CommandStatus::failed);
    }

    switch (mode) {
    case 1:
        return reset_settings(settings, restart, output, false, false);
    case 2:
        return reset_settings(settings, restart, output, false, true);
    case 3:
        return reset_calibration(restart, output);
    case 4:
        return reset_settings(settings, restart, output, true, false);
    case 5:
    case 6:
        return reset_settings(settings, restart, output, true, true);
    case 99:
        if (boot_counter == nullptr || !boot_counter->reset()) {
            return write_result(output, "Boot counter unavailable", CommandStatus::failed);
        }
        return schedule_restart(restart, output, false);
    default:
        return write_result(output, "1 to reset", CommandStatus::handled);
    }
}

} // namespace newmota::commands
