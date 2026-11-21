/*
  display.cpp - Beken display transport selection

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file display.cpp
 * @brief Beken panels use shared GPIO SPI, I80 or I2C transports.
 * The pinned BDK master forces additional pad muxes and unbounded semaphore
 * waits; native bus selectors are rejected before hardware is touched.
 */
#include "newmota/platform/display/native.h"

namespace newmota::platform::display::detail {
Result native_spi_preflight(const Config&) {
    return Result::unsupported;
}

Result native_spi_open(const Config&) {
    return Result::unsupported;
}

Result native_spi_write(bool, const std::uint8_t*, std::size_t) {
    return Result::unsupported;
}
} // namespace newmota::platform::display::detail
