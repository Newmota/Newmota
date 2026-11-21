/*
  deep_sleep.cpp - Pico deep sleep on the always-on timer

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file deep_sleep.cpp
 * @brief RP2350 powers the switched core down until the POWMAN alarm; RP2040 sleeps.
 *
 * RP2350: the POWMAN timer runs from the low-power oscillator, every domain powers down, and
 * the alarm boots the chip afresh, which CHIP_RESET reports as a switched-core power-down. The
 * record lives in the POWMAN scratch registers.
 *
 * RP2040 has no power-down with a timed wake: it stops every clock but the RTC's, waits for
 * the RTC alarm and then reboots through the watchdog. The record lives in uninitialized RAM,
 * which the reboot keeps, and marks the wake for restart_reason().
 */

#include "newmota/platform/deep_sleep.h"

#include "deep_sleep_wake.h"

#if defined(PICO_CYW43_ARCH_POLL) && PICO_CYW43_ARCH_POLL
#include "pico/cyw43_arch.h"
#endif
#include "hardware/sync.h"
#include "pico/aon_timer.h"
#include "pico/platform/sections.h"
#include "pico/stdlib.h"
#if PICO_RP2350
#include "hardware/powman.h"
#include "hardware/structs/powman.h"
#else
#include "hardware/structs/clocks.h"
#include "hardware/structs/nvic.h"
#include "hardware/structs/scb.h"
#include "hardware/watchdog.h"
#endif

#include <cstdint>
#include <ctime>

namespace newmota::platform {
namespace {

constexpr std::uint32_t kMagic = 0x4e4d4453U; // "NMDS"

struct Record final {
    std::uint32_t magic;
    deep_sleep::Retained state;
    std::uint32_t check;
};

[[nodiscard]] std::uint32_t checksum(const deep_sleep::Retained& state) {
    return ~(kMagic ^ state.next_wakeup ^ (state.slip << 7U) ^ (state.remaining << 13U));
}

#if PICO_RP2350
// POWMAN scratch 0..4: magic, wake, slip, remaining, check.
[[nodiscard]] Record read_record() {
    return {powman_hw->scratch[0],
        {powman_hw->scratch[1], powman_hw->scratch[2], powman_hw->scratch[3]},
        powman_hw->scratch[4]};
}

void write_record(const Record& record) {
    powman_hw->scratch[0] = record.magic;
    powman_hw->scratch[1] = record.state.next_wakeup;
    powman_hw->scratch[2] = record.state.slip;
    powman_hw->scratch[3] = record.state.remaining;
    powman_hw->scratch[4] = record.check;
}
#else
constexpr std::uint32_t kWakeMagic = 0x4e4d5741U; // "NMWA"

Record __uninitialized_ram(retained_record);
std::uint32_t __uninitialized_ram(wake_marker);
volatile bool alarm_fired = false;

[[nodiscard]] Record read_record() {
    return retained_record;
}

void write_record(const Record& record) {
    retained_record = record;
}

void on_alarm() {
    alarm_fired = true;
}
#endif

} // namespace

namespace pico_detail {

bool woke_from_deep_sleep() {
#if PICO_RP2350
    return (powman_hw->chip_reset & POWMAN_CHIP_RESET_HAD_SWCORE_PD_BITS) != 0U;
#else
    static bool known = false;
    static bool woke = false;
    if (!known) {
        woke = wake_marker == kWakeMagic;
        wake_marker = 0U;
        known = true;
    }
    return woke;
#endif
}

} // namespace pico_detail

namespace deep_sleep {

bool load(Retained& state) {
    const auto record = read_record();
    if (record.magic != kMagic || record.check != checksum(record.state))
        return false;
    state = record.state;
    return true;
}

void store(const Retained& state) {
    write_record({kMagic, state, checksum(state)});
}

bool retained() {
    return true;
}

std::uint32_t maximum_seconds() {
    return UINT32_MAX;
}

void enter(const std::uint64_t microseconds, bool) {
#if defined(PICO_CYW43_ARCH_POLL) && PICO_CYW43_ARCH_POLL
    // Switches the radio's power off; a wake that sleeps on at boot never started it.
    if (cyw43_is_initialized(&cyw43_state))
        cyw43_arch_deinit();
#endif
    const std::uint64_t milliseconds = (microseconds + 999U) / 1000U;
#if PICO_RP2350
    (void)save_and_disable_interrupts();
    powman_timer_stop();
    powman_timer_set_1khz_tick_source_lposc();
    powman_timer_set_ms(0U);
    powman_timer_start();
    powman_disable_all_wakeups();
    if (milliseconds != 0U)
        powman_enable_alarm_wakeup_at_ms(milliseconds);
    const powman_power_state off = POWMAN_POWER_STATE_NONE;
    powman_power_state on = powman_power_state_with_domain_on(
        POWMAN_POWER_STATE_NONE, POWMAN_POWER_DOMAIN_SWITCHED_CORE);
    on = powman_power_state_with_domain_on(on, POWMAN_POWER_DOMAIN_XIP_CACHE);
    on = powman_power_state_with_domain_on(on, POWMAN_POWER_DOMAIN_SRAM_BANK0);
    on = powman_power_state_with_domain_on(on, POWMAN_POWER_DOMAIN_SRAM_BANK1);
    powman_set_debug_power_request_ignored(true);
    if (powman_configure_wakeup_state(off, on)) {
        // Boot normally on wake instead of resuming at a saved address.
        for (auto& word : powman_hw->boot)
            word = 0U;
        (void)powman_set_power_state(off);
    }
    while (true)
        __wfi();
#else
    (void)save_and_disable_interrupts();
    nvic_hw->icer = ~0U;
    if (milliseconds != 0U) {
        const timespec start{};
        timespec alarm{};
        alarm.tv_sec = static_cast<time_t>((milliseconds + 999U) / 1000U);
        (void)aon_timer_start(&start);
        (void)aon_timer_enable_alarm(&alarm, on_alarm, true);
    }
    // Only the RTC keeps its clock; SLEEPDEEP gates every other one at the next WFI.
    clocks_hw->sleep_en0 = CLOCKS_SLEEP_EN0_CLK_RTC_RTC_BITS;
    clocks_hw->sleep_en1 = 0U;
    scb_hw->scr |= M0PLUS_SCR_SLEEPDEEP_BITS;
    __asm volatile("cpsie i" : : : "memory");
    while (!alarm_fired)
        __wfi();
    wake_marker = kWakeMagic;
    watchdog_reboot(0U, 0U, 0U);
    while (true) {}
#endif
}

} // namespace deep_sleep
} // namespace newmota::platform
