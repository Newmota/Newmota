/*
  status_commands.h - status command interface for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "core/command/command.h"

#include <cstdint>
#include <string_view>

namespace newmota {

class Settings;
class TimeService;
class PowerService;
struct BootStatus;

namespace commands {

/** @brief Writes the fields of Tasmota's STATE, without braces. */
using StateWriter = bool (*)(CommandOutput& output);

/**
 * @brief Lets StatusSTS report the full STATE; without a writer it reports the sleep and
 * power fields only (Safeboot).
 */
void set_state_writer(StateWriter writer);

/** @brief Live MQTT identity that only the running client knows; empty without a runtime. */
struct MqttStatus final {
    std::string_view topic{};
    std::string_view client{};
    std::uint32_t connections{};
};

using MqttStatusReader = MqttStatus (*)();

/** @brief Lets Status and StatusMQT report the resolved topic, client and connection count. */
void set_mqtt_status_reader(MqttStatusReader reader);

/** @brief Records the fallback routing of the MQTT command about to run. */
void set_status_request_fallback(bool fallback);

/**
 * @brief Completes a Status 0 request from the last dispatch. A combined response does not fit
 * one MQTT message, so MQTT receives its sections one at a time instead. A request during a
 * split runs once after it, so every requester sees complete sections.
 * @param deliver false for a suppressed command, which publishes nothing.
 * @return true when the last dispatch made such a request.
 */
[[nodiscard]] bool finish_status_split_request(bool deliver);

/**
 * @brief Removes the next section of a pending Status 0 split.
 * @param fallback Receives the routing of the request this section answers.
 * @return false when no section is pending.
 */
[[nodiscard]] bool next_status_split_section(std::uint8_t& section, bool& fallback);

/**
 * @brief Selects the MQTT topic suffix for a response when it is queued, right after dispatch.
 * Sections 8 and 10 both write StatusSNS, so only the dispatcher knows which one was requested.
 * @return STATUS8 or STATUS10 for a StatusSNS response, otherwise @p suffix. Safeboot's
 * StatusSNS is always empty, so it keeps @p suffix there too.
 */
[[nodiscard]] std::string_view status_response_suffix(
    std::string_view payload, std::string_view suffix);

/**
 * @brief Dispatches Tasmota-compatible Status commands.
 * @return The handler status, or unknown when the request is unrelated.
 */
[[nodiscard]] CommandStatus dispatch_status(const CommandRequest& request, const Settings& settings,
    const PowerService& power, const TimeService* time, const BootStatus* boot,
    CommandOutput& output);

} // namespace commands
} // namespace newmota
