/*
  ethernet_service.h - Tasmota's Ethernet settings and commands for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ethernet_service.h
 * @brief Ported from Tasmota's xdrv_82_esp32_ethernet.ino.
 *
 * The template's ETH MDC, MDIO and power pins, and for SPI controllers the SPI bus whose
 * MDC instance is used, select the hardware. Ethernet, EthType, EthAddress and EthClockMode
 * restart the device as in Tasmota; EthIPAddress, EthGateway, EthSubnetmask and
 * EthDNSServer1/2 apply at once. The settings live in `/.newmota/ethernet.bin`.
 */
#pragma once
#include "core/command/command.h"
#include "core/hardware/hardware.h"
#include "newmota/platform/network.h"

#include <string_view>

namespace newmota {
class RestartController;
} // namespace newmota

namespace newmota::ethernet {

/** @brief Loads the settings and starts the interface; after Wi-Fi set up the stack. */
void initialize(const Hardware& hardware, std::string_view hostname, RestartController& restart);

/** @brief Handles the Eth commands; unknown for anything else. */
[[nodiscard]] CommandStatus command(const CommandRequest& request, CommandOutput& output);

/** @brief Whether the wired interface has an address. */
[[nodiscard]] bool connected();

/** @brief Addressing of the wired interface while it runs, for StatusNET. */
[[nodiscard]] bool info(platform::NetworkInfo& info);

} // namespace newmota::ethernet
