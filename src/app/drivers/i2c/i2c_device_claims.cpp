/*
  i2c_device_claims.cpp - Cooperative I2C slave-address ownership

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file i2c_device_claims.cpp
 * @brief Retains exclusive logical slave leases for cooperating I2C drivers.
 */
#include "drivers/i2c/i2c_device_claims.h"
#include "newmota/platform/i2c.h"
#include <array>

namespace newmota::i2c_devices {
namespace {
struct Entry {
    std::uint32_t generation{};
    std::uint8_t bus{}, address{};
};

std::array<Entry, 16> entries{};
std::uint32_t generation{};
} // namespace

bool claim(std::uint8_t bus, std::uint8_t address, Lease& lease) {
    if (lease.generation || !address || address > 0x7f || !platform::i2c::available(bus) ||
        generation == UINT32_MAX)
        return false;
    Entry* free{};
    for (auto& entry : entries) {
        if (entry.generation && entry.bus == bus && entry.address == address)
            return false;
        if (!entry.generation && !free)
            free = &entry;
    }
    if (!free)
        return false;
    *free = {++generation, bus, address};
    lease = {generation};
    return true;
}

void release(Lease lease) {
    for (auto& entry : entries)
        if (lease.generation && entry.generation == lease.generation) {
            entry = {};
            return;
        }
}
} // namespace newmota::i2c_devices
