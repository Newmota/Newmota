/*
  ble_bond.h - bounded AmebaD BLE bond record validation

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ble_bond.h
 * @brief Canonical records for the pinned Realtek key-storage mask, without SDK types.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace newmota::platform::amebad_ble {
constexpr std::uint16_t kBondMask = 0x0F;
constexpr std::size_t kNativeBondSize = 96;

/** @brief Byte-only envelope; native padding is canonicalized before export. */
struct BondRecord {
    std::uint8_t magic[4], revision[40], mask[2], local_type, local_address[6];
    std::uint8_t keys[kNativeBondSize];
};

static_assert(sizeof(BondRecord) == 149);

/** @brief Validates a public, random static, or over-the-air peer resolvable address. */
inline bool valid_address(const std::uint8_t* address, std::uint8_t type, bool identity = true) {
    if (type > 1)
        return false;
    bool zero = true, ones = true;
    for (unsigned i = 0; i < 6; ++i) {
        zero &= address[i] == 0;
        ones &= address[i] == 0xFF;
    }
    if (zero || ones)
        return false;
    if (type == 0)
        return true;
    const auto kind = address[5] >> 6;
    if (!identity && kind == 1)
        return true;
    if (kind != 3)
        return false;
    zero = true;
    ones = true;
    for (unsigned i = 0; i < 6; ++i) {
        const auto mask = i == 5 ? 0x3F : 0xFF;
        zero &= (address[i] & mask) == 0;
        ones &= (address[i] & mask) == mask;
    }
    return !zero && !ones;
}

/** @brief Removes SDK padding and absent key fields from a native export. */
inline void canonicalize(std::uint8_t* keys) {
    for (const unsigned offset : {8U, 40U}) {
        if (keys[offset] == 0)
            std::memset(keys + offset, 0, 32);
        else
            std::memset(keys + offset + 1, 0, 3);
    }
    if (keys[95] == 0)
        std::memset(keys + 72, 0, 24);
}

/** @brief Checks lengths, key types, address types, masks and all reserved bytes. */
inline bool valid_keys(const std::uint8_t* keys, std::uint8_t local_type) {
    if (!valid_address(keys, keys[6], false) || (keys[7] & ~7U) != 0 || (keys[7] & 1) == 0 ||
        ((keys[7] >> 2) & 1) != local_type)
        return false;
    bool present = false;
    for (const unsigned offset : {8U, 40U}) {
        if (keys[offset] == 0) {
            for (unsigned i = 1; i < 32; ++i)
                if (keys[offset + i] != 0)
                    return false;
            continue;
        }
        const auto length = keys[offset + 30], type = keys[offset + 31];
        if (keys[offset] != 28 || keys[offset + 1] != 0 || keys[offset + 2] != 0 ||
            keys[offset + 3] != 0 || length < 7 || length > 16 ||
            (type != 4 && type != 5 && type != 7 && type != 8) ||
            ((type == 7 || type == 8) && length != 16))
            return false;
        present = true;
    }
    if (!present || keys[95] > 1)
        return false;
    if (keys[95] != 0)
        return valid_address(keys + 88, keys[94]);
    for (unsigned i = 72; i < 96; ++i)
        if (keys[i] != 0)
            return false;
    return keys[6] != 1 || (keys[5] >> 6) == 3;
}

/** @brief Binds a restored record to this exact SDK, storage configuration and identity. */
inline bool valid_record(const BondRecord& record, const char* revision, std::uint8_t local_type,
    const std::uint8_t* local_address) {
    return std::memcmp(record.magic, "ADK1", 4) == 0 &&
           std::memcmp(record.revision, revision, 40) == 0 && record.mask[0] == kBondMask &&
           record.mask[1] == 0 && record.local_type == local_type &&
           std::memcmp(record.local_address, local_address, 6) == 0 &&
           valid_address(record.local_address, local_type) && valid_keys(record.keys, local_type);
}
} // namespace newmota::platform::amebad_ble
