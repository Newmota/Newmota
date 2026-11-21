/*
  dhcp_server.h - Pico SDK SoftAP DHCP service

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

struct netif;

namespace newmota::platform::pico {

/** @brief Starts the AP-bound DHCP server; caller must hold the CYW43 lwIP lock. */
[[nodiscard]] bool start_dhcp_server(struct netif* access_point);

/** @brief Stops the DHCP server before its bound AP netif is removed. */
void stop_dhcp_server();

} // namespace newmota::platform::pico
