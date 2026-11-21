/*
  deep_sleep.cpp - AmebaD deep sleep through the KM0 power controller

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file deep_sleep.cpp
 * @brief KM4 asks the SDK's KM0 firmware for deep sleep with its AON timer, as the SDK
 * monitor's `dslp` command does; the wake is a boot with BIT_BOOT_DSLP_RESET_HAPPEN.
 *
 * The record lives in the AON backup registers the SDK leaves to users: REG1 and REG4 hold the
 * wake time and the remaining sleep, REG5 the slip and a check over all three.
 */

#include "newmota/platform/deep_sleep.h"

#include <cstdint>

extern "C" {
#include "ameba_soc.h"
}

namespace newmota::platform::deep_sleep {
namespace {

constexpr std::uint32_t kMagic = 0x4e4dU;

/** @brief Read by KM0 after the IPC; it must stay valid until the power goes. */
KM4SLEEP_ParamDef request;

[[nodiscard]] std::uint32_t check(const Retained& state) {
    const std::uint32_t folded = state.next_wakeup ^ state.remaining ^ state.slip;
    return (kMagic ^ folded ^ (folded >> 16U)) & 0xffffU;
}

} // namespace

bool load(Retained& state) {
    const std::uint32_t packed = BKUP_Read(BKUP_REG5);
    Retained candidate{};
    candidate.next_wakeup = BKUP_Read(BKUP_REG1);
    candidate.remaining = BKUP_Read(BKUP_REG4);
    candidate.slip = packed & 0xffffU;
    if (packed >> 16U != check(candidate))
        return false;
    state = candidate;
    return true;
}

void store(const Retained& state) {
    // The slip stays within 9000..11000, so 16 bits hold it.
    Retained stored = state;
    stored.slip &= 0xffffU;
    BKUP_Write(BKUP_REG1, stored.next_wakeup);
    BKUP_Write(BKUP_REG4, stored.remaining);
    BKUP_Write(BKUP_REG5, check(stored) << 16U | stored.slip);
}

bool retained() {
    return true;
}

std::uint32_t maximum_seconds() {
    // The AON timer takes milliseconds in 32 bits.
    return UINT32_MAX / 1000U;
}

void enter(const std::uint64_t microseconds, bool) {
    std::uint64_t milliseconds = (microseconds + 999U) / 1000U;
    if (milliseconds > UINT32_MAX)
        milliseconds = UINT32_MAX;
    request.dlps_enable = TRUE;
    request.sleep_type = 0U;
    request.sleep_time = static_cast<u32>(milliseconds);
    DCache_Clean(reinterpret_cast<u32>(&request), sizeof request);

    asm volatile("cpsid i" : : : "memory");
    ipc_send_message(IPC_INT_KM4_TICKLESS_INDICATION, reinterpret_cast<u32>(&request));
    asm volatile("wfe");
    asm volatile("wfe");
    while (true) {}
}

} // namespace newmota::platform::deep_sleep
