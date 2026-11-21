/*
  amebad_ble_bond.cpp - persistent BLE credential record validation

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file amebad_ble_bond.cpp
 * @brief Rejects malformed or incompatible restored credential records without an SDK.
 */
#include "ble_bond.h"
#include <cassert>
#include <cstring>

namespace {
using namespace newmota::platform::amebad_ble;
constexpr char revision[] = "0123456789012345678901234567890123456789";
constexpr std::uint8_t identity[]{1, 2, 3, 4, 5, 6};

BondRecord record() {
    BondRecord result{};
    std::memcpy(result.magic, "ADK1", 4);
    std::memcpy(result.revision, revision, 40);
    result.mask[0] = kBondMask;
    std::memcpy(result.local_address, identity, 6);
    std::memcpy(result.keys, identity, 6);
    result.keys[7] = 3;
    result.keys[8] = 28;
    result.keys[12] = 0xAB;
    result.keys[38] = 16;
    result.keys[39] = 8;
    return result;
}

bool valid(const BondRecord& value) {
    return valid_record(value, revision, 0, identity);
}
} // namespace

int main() {
    const auto original = record();
    assert(valid(original));
    // Each mutation crosses a durable format or security metadata boundary.
    for (const unsigned offset :
        {0U, 4U, 44U, 45U, 46U, 47U, 59U, 60U, 61U, 62U, 63U, 91U, 92U, 148U}) {
        auto changed = original;
        reinterpret_cast<std::uint8_t*>(&changed)[offset] ^= 0x80;
        assert(!valid(changed));
    }
    auto changed = original;
    changed.keys[38] = 15; // Secure Connections requires a 16-byte key.
    assert(!valid(changed));
    changed = original;
    changed.keys[39] = 4; // Legacy keys permit shorter negotiated key lengths.
    changed.keys[38] = 7;
    assert(valid(changed));
    changed.keys[38] = 6;
    assert(!valid(changed));
    changed = original;
    changed.keys[8] = 0;
    canonicalize(changed.keys);
    assert(!valid(changed)); // A bond without either LTK cannot be restored.
    changed = original;
    changed.keys[9] = 0xA5;
    changed.keys[41] = 0xA5;
    changed.keys[72] = 0xA5;
    assert(!valid(changed));
    canonicalize(changed.keys);
    assert(std::memcmp(&changed, &original, sizeof(original)) == 0);
    changed = original;
    changed.keys[6] = 1;
    changed.keys[5] = 0x45; // A resolvable peer needs its distributed identity.
    assert(!valid(changed));
    changed.keys[95] = 1;
    std::memcpy(changed.keys + 88, identity, 6);
    assert(valid(changed));
    changed.keys[94] = 2;
    assert(!valid(changed));
    changed = original;
    changed.local_type = 1;
    changed.local_address[5] = 0xC6;
    changed.keys[7] = 7;
    assert(valid_record(changed, revision, 1, changed.local_address));
    assert(!valid_record(changed, revision, 0, changed.local_address));
    changed.local_address[5] = 0x86;
    assert(!valid_record(changed, revision, 1, changed.local_address));
}
