/*
  sd_card_unavailable.cpp - Unavailable SD card adapter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file sd_card_unavailable.cpp
 * @brief SD card adapter for families without an SD transport yet.
 */

#include "newmota/platform/sd_card.h"

namespace newmota::platform {
SdCardResult open_sd_card(const SdCardConfig&, SectorDevice& device) {
    device = {};
    return SdCardResult::unsupported;
}
} // namespace newmota::platform
