/*
  mcp23008.h - Owned MCP23008 register access

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file mcp23008.h
 * @brief Exclusive portable MCP23008 access without implicit relay/direction changes.
 */
#pragma once
#include "drivers/i2c/i2c_device_claims.h"

namespace newmota::io_expander {
/** @brief A cooperative MCP23008 slave owner; reads fail closed on I2C errors.
 * Opening does not alter IODIR, polarity, pull-ups or output latches. Board/relay
 * integration must configure those explicitly through this owner before use.
 */
class Mcp23008 {
    i2c_devices::Lease lease_{};
    std::uint8_t bus_{}, address_{};

  public:
    Mcp23008() = default;
    Mcp23008(const Mcp23008&) = delete;
    Mcp23008& operator=(const Mcp23008&) = delete;
    ~Mcp23008();
    /** @brief Claims a zero-based I2C bus/address 0x20..0x27 and probes the slave. */
    bool open(std::uint8_t bus, std::uint8_t address);
    /** @brief Reads one register 0x00..0x0A; false leaves the destination unchanged. */
    bool read(std::uint8_t reg, std::uint8_t& value) const;
    /** @brief Writes a writable register; interrupt flags/capture are read-only. */
    bool write(std::uint8_t reg, std::uint8_t value);
    /** @brief Reads GPIO(0x09); callers must not substitute a stale snapshot on failure. */
    bool snapshot(std::uint8_t& pins) const;
};
} // namespace newmota::io_expander
