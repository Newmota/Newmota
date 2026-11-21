/*
  flash_capacity.cpp - host tests for proven flash capacity resolution

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/flash_capacity.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

using newmota::platform::resolve_flash_capacity;

constexpr std::uint32_t kMiB = 1024U * 1024U;

/** @brief Resolves against a chip of real_bytes whose addresses wrap modulo its size. */
std::uint32_t resolve(const std::uint32_t declared, const std::uint8_t code,
    const std::uint32_t real_bytes, const std::uint32_t limit = 16U * kMiB) {
    return resolve_flash_capacity(declared, code, limit,
        [real_bytes](const std::uint32_t offset) { return offset % real_bytes == 0U; });
}

} // namespace

int main() {
    // Honest chips: grow to the reported size, or stay at the catalog size.
    assert(resolve(2U * kMiB, 0x15U, 2U * kMiB) == 2U * kMiB);
    assert(resolve(2U * kMiB, 0x16U, 4U * kMiB) == 4U * kMiB);
    assert(resolve(2U * kMiB, 0x18U, 16U * kMiB) == 16U * kMiB);

    // An overstated capacity code is cut back at the first mirror of offset 0.
    assert(resolve(2U * kMiB, 0x18U, 2U * kMiB) == 2U * kMiB);
    assert(resolve(2U * kMiB, 0x18U, 4U * kMiB) == 4U * kMiB);

    // An understated or absent code never shrinks below the catalog size.
    assert(resolve(2U * kMiB, 0x14U, 2U * kMiB) == 2U * kMiB);
    assert(resolve(2U * kMiB, 0x00U, 2U * kMiB) == 2U * kMiB);
    assert(resolve(2U * kMiB, 0xFFU, 16U * kMiB) == 2U * kMiB);

    // Sizes beyond the addressing limit are clamped to it.
    assert(resolve(2U * kMiB, 0x19U, 32U * kMiB) == 16U * kMiB);
    assert(resolve(4U * kMiB, 0x19U, 32U * kMiB, 4U * kMiB) == 4U * kMiB);

    // A failed probe read reports a mirror and stops growth there.
    assert(resolve_flash_capacity(2U * kMiB, 0x18U, 16U * kMiB,
               [](const std::uint32_t offset) { return offset == 4U * kMiB; }) == 4U * kMiB);

    // Unusable inputs keep the catalog size.
    assert(resolve(3U * kMiB, 0x18U, 16U * kMiB) == 3U * kMiB);
    assert(resolve(0U, 0x18U, 16U * kMiB) == 0U);
    assert(resolve(2U * kMiB, 0x18U, 16U * kMiB, 3U * kMiB) == 2U * kMiB);
    std::puts("Flash capacity tests passed");
}
