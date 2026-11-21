/*
  matter_ble.h - Matter commissioning link over the BLE server

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "newmota/platform/matter_udp.h"

#include <cstddef>
#include <cstdint>

namespace newmota::matter {

/** @brief Starts BLE and advertises as commissionable. @return False when BLE cannot start. */
[[nodiscard]] bool ble_link_start(
    std::uint16_t discriminator, std::uint16_t vendor, std::uint16_t product);
/** @brief Closes any connection and releases the BLE stack. Blocks while it closes. */
void ble_link_stop();
/** @brief Advances the GATT setup and moves pending indications and writes. */
void ble_link_poll(std::uint32_t now);
/** @brief Takes one reassembled message as a datagram for matter_udp_release, or nullptr. */
[[nodiscard]] platform::UdpDatagram* ble_link_take();
/** @brief Queues one message for the connected peer. */
[[nodiscard]] bool ble_link_send(const std::uint8_t* data, std::size_t size);

} // namespace newmota::matter
