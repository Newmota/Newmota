/*
  mcp23008.cpp - Owned MCP23008 register access

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file mcp23008.cpp
 * @brief Owns one MCP23008 address and performs bounded atomic register transactions.
 */
#include "drivers/io_expander/mcp23008.h"
#include "newmota/platform/i2c.h"

namespace newmota::io_expander {
Mcp23008::~Mcp23008() {
    i2c_devices::release(lease_);
}

bool Mcp23008::open(std::uint8_t bus, std::uint8_t address) {
    if (lease_.generation || address < 0x20 || address > 0x27 ||
        !i2c_devices::claim(bus, address, lease_))
        return false;
    bus_ = bus;
    address_ = address;
    std::uint8_t configuration{};
    if (read(0x05, configuration))
        return true;
    i2c_devices::release(lease_);
    lease_ = {};
    return false;
}

bool Mcp23008::read(std::uint8_t reg, std::uint8_t& value) const {
    std::uint8_t result{};
    if (!lease_.generation || reg > 0x0a ||
        platform::i2c::transfer(bus_, address_, &reg, 1, &result, 1) !=
            platform::i2c::Result::success)
        return false;
    value = result;
    return true;
}

bool Mcp23008::write(std::uint8_t reg, std::uint8_t value) {
    const std::uint8_t data[]{reg, value};
    return lease_.generation && reg <= 0x0a && reg != 7 && reg != 8 &&
           platform::i2c::transfer(bus_, address_, data, 2, nullptr, 0) ==
               platform::i2c::Result::success;
}

bool Mcp23008::snapshot(std::uint8_t& pins) const {
    return read(0x09, pins);
}
} // namespace newmota::io_expander
