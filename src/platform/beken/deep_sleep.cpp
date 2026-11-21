/*
  deep_sleep.cpp - Beken RTC deep sleep

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file deep_sleep.cpp
 * @brief The BDK's deep sleep with its RTC wake, timed by the internal 32 kHz oscillator.
 *
 * The wake is a reset the SDK reports as RESET_SOURCE_DEEPPS_RTC. RAM is lost and the only
 * retention register belongs to the SDK's reset-source record, so no state is kept.
 */

#include "newmota/platform/deep_sleep.h"

#include <cstring>

extern "C" {
#include "manual_ps_pub.h"
#include "sys_ctrl_pub.h"
}

namespace newmota::platform::deep_sleep {
namespace {

/** @brief The BDK caps the RTC wake at 0x1ffff seconds. */
constexpr std::uint32_t kMaximumSeconds = 0x1ffffU;

} // namespace

bool load(Retained&) {
    return false;
}

void store(const Retained&) {}

bool retained() {
    return false;
}

std::uint32_t maximum_seconds() {
    return kMaximumSeconds;
}

void enter(const std::uint64_t microseconds, bool) {
    PS_DEEP_CTRL_PARAM parameters;
    std::memset(&parameters, 0, sizeof parameters);
    if (microseconds != 0U) {
        // Whole seconds, rounded up so a short request still sets a wake.
        const std::uint64_t seconds = (microseconds + 999999U) / 1000000U;
        parameters.wake_up_way = PS_DEEP_WAKEUP_RTC;
        parameters.sleep_time =
            static_cast<UINT32>(seconds < kMaximumSeconds ? seconds : kMaximumSeconds);
        parameters.lpo_32k_src = LPO_SELECT_ROSC;
    }
    bk_enter_deep_sleep_mode(&parameters);
    while (true) {}
}

} // namespace newmota::platform::deep_sleep
