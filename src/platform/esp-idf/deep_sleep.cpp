/*
  deep_sleep.cpp - ESP-IDF deep sleep and RTC-retained wake state

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file deep_sleep.cpp
 * @brief Timer and ULP wake through esp_sleep; the record lives in no-init RTC memory.
 *
 * The PHY's deep-sleep hook switches the radio off, so the caller only closes its sessions.
 * The ESP32-C2 has no RTC memory, so it sleeps without a retained record.
 */

#include "newmota/platform/deep_sleep.h"

#include "sdkconfig.h"

#include "esp_attr.h"
#include "esp_sleep.h"
#include "soc/soc_caps.h"

#include <cstdint>
#include <limits>

#if SOC_RTC_FAST_MEM_SUPPORTED || SOC_RTC_SLOW_MEM_SUPPORTED
#define NEWMOTA_DEEP_SLEEP_RETAINED 1
#else
#define NEWMOTA_DEEP_SLEEP_RETAINED 0
#endif

namespace newmota::platform::deep_sleep {
namespace {

#if NEWMOTA_DEEP_SLEEP_RETAINED
constexpr std::uint32_t kMagic = 0x4e4d4453U; // "NMDS"

struct Record final {
    std::uint32_t magic;
    Retained state;
    std::uint32_t check;
};

RTC_NOINIT_ATTR Record record;

/** @brief Folds the fields so stale or power-on RTC contents fail the check. */
[[nodiscard]] std::uint32_t checksum(const Retained& state) {
    return ~(kMagic ^ state.next_wakeup ^ (state.slip << 7U) ^ (state.remaining << 13U));
}
#endif

} // namespace

bool load(Retained& state) {
#if NEWMOTA_DEEP_SLEEP_RETAINED
    if (record.magic != kMagic || record.check != checksum(record.state))
        return false;
    state = record.state;
    return true;
#else
    (void)state;
    return false;
#endif
}

void store(const Retained& state) {
#if NEWMOTA_DEEP_SLEEP_RETAINED
    record.magic = kMagic;
    record.state = state;
    record.check = checksum(state);
#else
    (void)state;
#endif
}

bool retained() {
    return NEWMOTA_DEEP_SLEEP_RETAINED != 0;
}

std::uint32_t maximum_seconds() {
    // The RTC timer counts far beyond 32 bits of seconds.
    return std::numeric_limits<std::uint32_t>::max();
}

void enter(const std::uint64_t microseconds, const bool coprocessor_wake) {
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (microseconds != 0U)
        (void)esp_sleep_enable_timer_wakeup(microseconds);
#if CONFIG_ULP_COPROC_ENABLED
    if (coprocessor_wake)
        (void)esp_sleep_enable_ulp_wakeup();
#else
    (void)coprocessor_wake;
#endif
    esp_deep_sleep_start();
}

} // namespace newmota::platform::deep_sleep
