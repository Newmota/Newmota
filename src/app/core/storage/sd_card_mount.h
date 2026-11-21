/*
  sd_card_mount.h - boot-time SD card mount from the GPIO configuration

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

namespace newmota {

class Hardware;

/**
 * @brief Mounts the SD card at `/sd` from Tasmota's SD GPIO functions and logs the outcome.
 *
 * As in Tasmota, an assigned SDCard CS selects SPI mode on its bus (instance 2 wins over
 * instance 1); otherwise SDIO CMD, CLK and D0 select SDIO, four-bit with D1 to D3. Without
 * those pins nothing is mounted or logged. The card is never formatted.
 */
void mount_sd_card(const Hardware& hardware);

} // namespace newmota
