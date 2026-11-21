/*
  i2c_device_claims.h - Cooperative I2C slave-address ownership

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file i2c_device_claims.h
 * @brief Prevents cooperating protocol owners from binding the same physical slave.
 */
#pragma once
#include <cstdint>

namespace newmota::i2c_devices {
/** @brief A non-reused generation identifying one bus/address reservation. */
struct Lease {
    std::uint32_t generation{};
};

/** @brief Claims one zero-based bus and seven-bit address; at most 16 leases coexist.
 * Cooperative application-task use only. This does not block raw I2C transfers.
 * @return true when the available bus/address was reserved exclusively.
 */
bool claim(std::uint8_t bus, std::uint8_t address, Lease& lease);
/** @brief Releases only the matching generation; stale releases cannot free a new owner. */
void release(Lease lease);
} // namespace newmota::i2c_devices
