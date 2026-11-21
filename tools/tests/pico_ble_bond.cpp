/*
  pico_ble_bond.cpp - Pico durable BLE credential validation

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file pico_ble_bond.cpp
 * @brief Rejects incompatible identities, malformed key metadata and truncated records.
 */
#include "ble_bond.h"
#include <cassert>
#include <cstring>
#include <initializer_list>

using namespace newmota::platform::pico_ble;

int main() {
    constexpr char revision[] = "0123456789012345678901234567890123456789";
    const std::uint8_t local[6]{1, 2, 3, 4, 5, 6}, zero[6]{};
    BondRecord original{};
    std::memcpy(original.magic, "PBK1", 4);
    std::memcpy(original.revision, revision, 40);
    std::memcpy(original.local, local, 6);
    std::memcpy(original.peer, local, 6);
    original.key_size = 16;
    original.secure_connections = 1;
    const auto valid = [&](const BondRecord& record) {
        return valid_record(record, revision, local, 0, zero);
    };
    assert(valid(original));
    for (unsigned i = 0; i < 50; ++i) {
        auto changed = original;
        reinterpret_cast<std::uint8_t*>(&changed)[i] ^= 0x80;
        assert(!valid(changed));
    }
    auto changed = original;
    changed.key_size = 15;
    assert(!valid(changed));
    changed.secure_connections = 0;
    assert(valid(changed));
    changed.key_size = 6;
    assert(!valid(changed));
    changed.key_size = 0;
    assert(valid(changed)); // Legacy peripheral bond without a peer-distributed LTK.
    changed.ltk[0] = 1;
    assert(!valid(changed));
    changed.ltk[0] = 0;
    changed.random[7] = 1;
    assert(!valid(changed));
    changed.random[7] = 0;
    changed.ediv[0] = 1;
    assert(!valid(changed));
    changed.ediv[0] = 0;
    changed.secure_connections = 1;
    assert(!valid(changed));
    for (auto field : {&BondRecord::authenticated, &BondRecord::authorized,
             &BondRecord::secure_connections, &BondRecord::peer_type, &BondRecord::own_type}) {
        changed = original;
        changed.*field = 2;
        assert(!valid(changed));
    }
    changed = original;
    changed.peer_type = 1;
    assert(!valid(changed));
    changed.peer[0] |= 0xc0;
    assert(valid(changed));
    const std::uint8_t empty_random[6]{0xc0, 0, 0, 0, 0, 0};
    assert(!valid_identity(empty_random, 1));
    assert(!valid_identity(zero, 0));
    changed = original;
    changed.own_address[2] = 1;
    assert(!valid(changed));
    return 0;
}
