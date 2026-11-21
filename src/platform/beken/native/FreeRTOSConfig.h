/*
  FreeRTOSConfig.h - Beken FreeRTOS memory-stat API override

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file FreeRTOSConfig.h
 * @brief Beken FreeRTOS memory-stat API override.
 */

#pragma once

// Keep the pinned BDK configuration as the source of all target settings;
// Newmota only enables FreeRTOS's existing stack high-water API.
#include_next <FreeRTOSConfig.h>

#undef INCLUDE_uxTaskGetStackHighWaterMark
#define INCLUDE_uxTaskGetStackHighWaterMark 1
