/*
  led_strip.cpp - Beken LED strip adapter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file led_strip.cpp
 * @brief Beken LED strip adapter.
 */

#include "newmota/platform/led_strip.h"

namespace newmota::platform::led_strip {
// The pinned BDK SPI DMA path adds one clock between bytes, even with
// interval=0. With 1000/1100 encoding at 3.25 MHz its boundary T0L is 1.23 us,
// above WS2812B-V6's 1 us limit. No SPI/DMA/mux/timer is touched here.
Result open(PinId, std::size_t, Port&) {
    return Result::unsupported;
}

Result transmit(Port, const std::uint8_t*, std::size_t) {
    return Result::unsupported;
}

Result readiness(Port) {
    return Result::busy;
}

Result close(Port) {
    return Result::unsupported;
}
} // namespace newmota::platform::led_strip
