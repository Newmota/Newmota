/*
  hosted.h - ESP-Hosted Wi-Fi companion of the ESP32-P4 for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file hosted.h
 * @brief Ported from Tasmota's xdrv_84_esp32_hosted.ino.
 *
 * The P4 has no radio; an ESP32-C6 running Espressif's network_adapter firmware carries
 * Wi-Fi over SDIO, and ESP-IDF's esp_wifi calls become RPCs to it. The application sets the
 * template pins once, then the Wi-Fi adapter starts the transport. Every call belongs to the
 * application task and may block on an RPC.
 */
#pragma once
#include "newmota/core/hardware/pin.h"
#include <cstddef>
#include <cstdint>

namespace newmota::platform::hosted {

/** @brief SDIO pads to the companion; invalid ones keep the build's defaults. */
struct Pins final {
    PinId clk{}, cmd{}, d0{}, d1{}, d2{}, d3{}, reset{};
};

/** @brief Firmware version packed as major << 16 | minor << 8 | patch; 0 when unknown. */
using Version = std::uint32_t;

enum class Result : std::uint8_t { success, unavailable, failed };

/** @brief Replaces the default pins; only before the transport starts. */
void set_pins(const Pins& pins);

/** @brief Starts the transport and connects to the companion; repeated calls are no-ops. */
[[nodiscard]] Result start();

/** @brief Whether the transport is configured and running. */
[[nodiscard]] bool running();

/** @brief The ESP-Hosted host library version built in. */
[[nodiscard]] Version host_version();

/** @brief The companion's firmware version, asked once and cached. */
[[nodiscard]] Version companion_version();

/** @brief Writes the companion's chip name, such as "esp32c6", into @p name. */
void companion_chip(char* name, std::size_t capacity);

/** @brief Companion firmware update: begin, any number of writes, end, then activate. */
[[nodiscard]] Result update_begin();
[[nodiscard]] Result update_write(const std::uint8_t* data, std::size_t size);
[[nodiscard]] Result update_end();
/** @brief Boots the new image; the companion restarts and the link drops. */
[[nodiscard]] Result update_activate();

} // namespace newmota::platform::hosted
