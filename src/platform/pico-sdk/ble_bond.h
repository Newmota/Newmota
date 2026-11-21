/*
  ble_bond.h - Pico BLE credential envelope

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ble_bond.h
 * @brief Validates durable native security records without SDK structures.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace newmota::platform::pico_ble {
/** @brief Named-byte same-SDK security envelope; no native structs cross persistence. */
struct BondRecord {
    std::uint8_t magic[4], revision[40], local[6], own_type, own_address[6];
    std::uint8_t peer_type, peer[6], irk[16], ediv[2], random[8], ltk[16];
    std::uint8_t key_size, authenticated, authorized, secure_connections, er[16], ir[16];
};

static_assert(sizeof(BondRecord) == 142);

/** @brief Checks a native display-order public or static random identity. */
inline bool valid_identity(const std::uint8_t* address, std::uint8_t type) {
    if (type > 1 || (type == 1 && (address[0] & 0xc0) != 0xc0))
        return false;
    bool zero = true, ones = true;
    for (unsigned i = 0; i < 6; ++i) {
        const auto mask = i == 0 && type == 1 ? 0x3f : 0xff;
        zero &= (address[i] & mask) == 0;
        ones &= (address[i] & mask) == mask;
    }
    return !zero && !ones;
}

/** @brief Binds key metadata, native revision and both local identities. */
inline bool valid_record(const BondRecord& bond, const char* revision, const std::uint8_t* local,
    std::uint8_t own_type, const std::uint8_t* own_address) {
    if (std::memcmp(bond.magic, "PBK1", 4) || std::memcmp(bond.revision, revision, 40) ||
        std::memcmp(bond.local, local, 6) || !valid_identity(bond.local, 0) ||
        !valid_identity(bond.peer, bond.peer_type) || bond.own_type != own_type ||
        std::memcmp(bond.own_address, own_address, 6) || bond.key_size > 16 ||
        (bond.key_size != 0 && bond.key_size < 7) || bond.authenticated > 1 ||
        bond.authorized > 1 || bond.secure_connections > 1 ||
        (bond.secure_connections && bond.key_size != 16))
        return false;
    if (bond.key_size == 0) {
        // A legacy peripheral derives its own LTK from ER/IR when the central
        // distributed identity information without a remote encryption key.
        if (bond.authenticated || bond.authorized || bond.secure_connections)
            return false;
        for (const auto byte : bond.ediv)
            if (byte)
                return false;
        for (const auto byte : bond.random)
            if (byte)
                return false;
        for (const auto byte : bond.ltk)
            if (byte)
                return false;
    }
    if (own_type == 1)
        return valid_identity(bond.own_address, own_type);
    if (own_type != 0)
        return false;
    const std::uint8_t zero[6]{};
    return !std::memcmp(bond.own_address, zero, 6);
}
} // namespace newmota::platform::pico_ble
