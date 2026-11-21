/*
  ble_internal.h - ESP-IDF BLE adapter internals

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ble_internal.h
 * @brief What the stack, scan and connection files of the ESP-IDF BLE adapter share.
 */

#pragma once
#include "newmota/platform/ble.h"

#include <cstdint>

namespace newmota::platform::ble_detail {
/** @brief Own address type for connecting, scanning and advertising. */
extern std::uint8_t own_address_type;
/** @brief Reports whether the host is synchronised with the controller. */
[[nodiscard]] bool synced();

// ble_scan.cpp
void scan_init();
void scan_on_sync();
/** @brief Counts links; scanning pauses while any link exists. */
void scan_links(int delta);
/** @brief Stops scanning now and keeps it stopped until released. */
void scan_hold(bool hold);
[[nodiscard]] bool scan_running();
[[nodiscard]] std::uint16_t scan_stack_free();

// ble_connection.cpp
void connection_init();
/** @brief Closes the connection task and its links. @return False on timeout. */
[[nodiscard]] bool connection_shutdown();
/** @brief Fills the connection related parts of the info record. */
void connection_fill(BleInfo& info);

/** @brief Reverses a six byte address between display and host order. */
inline void reverse_address(const std::uint8_t* from, std::uint8_t* to) {
    for (int i = 0; i < 6; ++i)
        to[i] = from[5 - i];
}
} // namespace newmota::platform::ble_detail
