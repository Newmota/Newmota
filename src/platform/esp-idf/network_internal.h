/*
  network_internal.h - ESP-IDF internal Wi-Fi lifecycle declaration

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file network_internal.h
 * @brief ESP-IDF internal Wi-Fi lifecycle declaration.
 */

#pragma once

namespace newmota::platform {
/** @brief Reports whether the ESP-IDF Wi-Fi driver is currently running. */
bool wifi_driver_running();
} // namespace newmota::platform
