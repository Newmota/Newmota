/*
  network_commands.h - read-only network information commands for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "core/command/command.h"
#include "newmota/platform/network.h"

#include <cstddef>

namespace newmota {

class NetworkService;

namespace commands {

/**
 * @brief Dispatches read-only Hostname and IPAddress queries.
 *
 * When station addressing is unavailable, IPAddress uses the active SoftAP
 * address so the compatibility command remains useful during setup.
 * @return The handler status, or unknown for unrelated commands.
 */
[[nodiscard]] CommandStatus dispatch_network_queries(
    const CommandRequest& request, CommandOutput& output);

/** @brief Dispatches Tasmota-compatible WiFiScan and WifiTest commands. */
[[nodiscard]] CommandStatus dispatch_wifi_commands(
    const CommandRequest& request, NetworkService& network, CommandOutput& output);

/** @brief Serializes one asynchronous Tasmota-shaped scan announcement. */
[[nodiscard]] bool serialize_wifi_scan_announcement(std::size_t network_number,
    const platform::WifiScanRecord* result, char* output, std::size_t capacity, std::size_t& size);

} // namespace commands
} // namespace newmota
