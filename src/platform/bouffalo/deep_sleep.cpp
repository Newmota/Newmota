/*
  deep_sleep.cpp - Bouffalo HBN deep sleep with RTC wake

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file deep_sleep.cpp
 * @brief Hibernate (HBN level 0) until the 32768 Hz RC RTC compare fires, then a full boot.
 *
 * The HBN registers that survive belong to the SDK and the boot ROM, and the HBN RAM has no
 * qualified free range, so no state is kept. BL602 and BL702 take a 32-bit tick count, about
 * 36 hours; BL616 and BL618DG take a 64-bit one.
 */

#include "newmota/platform/deep_sleep.h"

#include <cstdint>
#include <limits>

extern "C" {
#if defined(BL602)
#include "bl602_hbn.h"
#elif defined(BL616)
#include "bl616_pm.h"
#elif defined(BL618DG)
#include "bl618dg_pm.h"
#elif defined(BL702)
#include "bl702_pm.h"
#endif
}

namespace newmota::platform::deep_sleep {
namespace {

constexpr std::uint64_t kTicksPerSecond = 32768U;
#if defined(BL616) || defined(BL618DG)
using Ticks = std::uint64_t;
#else
using Ticks = std::uint32_t;
#endif

} // namespace

bool load(Retained&) {
    return false;
}

void store(const Retained&) {}

bool retained() {
    return false;
}

std::uint32_t maximum_seconds() {
    const std::uint64_t seconds = std::numeric_limits<Ticks>::max() / kTicksPerSecond;
    return seconds < std::numeric_limits<std::uint32_t>::max()
               ? static_cast<std::uint32_t>(seconds)
               : std::numeric_limits<std::uint32_t>::max();
}

void enter(const std::uint64_t microseconds, bool) {
    std::uint64_t ticks = microseconds * kTicksPerSecond / 1000000U;
    if (microseconds != 0U && ticks == 0U)
        ticks = 1U;
    if (ticks > std::numeric_limits<Ticks>::max())
        ticks = std::numeric_limits<Ticks>::max();
#if defined(BL602)
    HBN_APP_CFG_Type config{};
    config.useXtal32k = 0U;
    config.sleepTime = static_cast<std::uint32_t>(ticks);
    config.gpioWakeupSrc = 0U;
    config.gpioTrigType = HBN_GPIO_INT_TRIGGER_ASYNC_FALLING_EDGE;
    config.flashCfg = nullptr;
    config.hbnLevel = HBN_LEVEL_0;
    config.ldoLevel = HBN_LDO_LEVEL_1P10V;
    HBN_Mode_Enter_Ext(&config);
#else
    pm_hbn_mode_enter(PM_HBN_LEVEL_0, static_cast<Ticks>(ticks));
#endif
    while (true) {}
}

} // namespace newmota::platform::deep_sleep
