/*
  i2s.cpp - Bouffalo SoC I2S capability boundary

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file i2s.cpp
 * @brief Reports the BL602 native peripheral limitation without GPIO emulation.
 * The pinned SDK's BL602 device table, memory map and interrupt map provide no
 * I2S device. Its LHAL build deliberately excludes bflb_i2s.c for BL602.
 * BL616/618 are different silicon and require their own qualified native route.
 */
#include "newmota/platform/i2s.h"

namespace newmota::platform::i2s {
Capabilities capabilities() {
    return {};
}

Result open(const Config&, Port&) {
    return Result::unsupported;
}

Result close(Port) {
    return Result::unsupported;
}

bool tx_idle(Port) {
    return true;
}

Transfer read(Port, std::uint8_t*, std::size_t) {
    return {Result::unsupported, 0};
}

Transfer write(Port, const std::uint8_t*, std::size_t) {
    return {Result::unsupported, 0};
}

Status status(Port) {
    return {};
}

void poll() {}
} // namespace newmota::platform::i2s
