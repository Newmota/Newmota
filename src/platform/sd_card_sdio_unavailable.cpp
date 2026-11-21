/*
  sd_card_sdio_unavailable.cpp - SDIO mode for families without an SD host

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file sd_card_sdio_unavailable.cpp
 * @brief SDIO mode where only the SPI-mode protocol exists.
 */

#include "newmota/platform/sd_card.h"

namespace newmota::platform::detail {
SdCardResult open_sdio_card(const SdCardConfig&, SectorDevice& device) {
    device = {};
    return SdCardResult::unsupported;
}
} // namespace newmota::platform::detail
