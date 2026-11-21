/*
  power_switches.cpp - Tasmota switch modes for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  SwitchHandler/SwitchState reference: Tasmota support_switch_v4.ino,
  Copyright (C) 2022 Federico Leoni and Theo Arends, GPL version 3 or later.
*/

/** @file power_switches.cpp
 * @brief Runs Tasmota's SwitchHandler over filtered switch levels and routes keys or Power.
 */
#include "services/power/power_service.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace newmota {
namespace {

// Tasmota SendKey states and switch levels; a pressed switch reads LOW.
constexpr std::uint32_t kPowerOff = 0U;
constexpr std::uint32_t kPowerOn = 1U;
constexpr std::uint32_t kPowerToggle = 2U;
constexpr std::uint32_t kPowerHold = 3U;
constexpr std::uint32_t kPowerIncrement = 4U;
constexpr std::uint32_t kPowerInv = 5U;
constexpr std::uint32_t kPowerClear = 6U;
constexpr std::uint32_t kPowerRelease = 7U;
constexpr std::uint32_t kPower100 = 8U;
constexpr std::uint32_t kPowerDelayed = 10U;
constexpr std::uint32_t kPowerNone = 99U;
constexpr std::uint32_t kPressed = 0U;
constexpr std::uint32_t kNotPressed = 1U;

// Press flags of the multi-press modes 11 and 12. Tasmota keeps them in bits 6 and 7 of the
// hold timer byte, where a count above 63 (the default 50 ms SwitchDebounce with SetOption32
// above 31) overlaps them; Newmota keeps flags and count apart.
constexpr std::uint8_t kFirstPress = 0x40U;
constexpr std::uint8_t kSecondPress = 0x80U;

enum SwitchMode : std::uint8_t {
    toggle,
    follow,
    follow_inv,
    pushbutton,
    pushbutton_inv,
    pushbutton_hold,
    pushbutton_hold_inv,
    pushbutton_toggle,
    toggle_multi,
    follow_multi,
    follow_multi_inv,
    push_hold_multi,
    push_hold_multi_inv,
    push_on,
    push_on_inv,
    push_ignore,
    push_ignore_inv,
};

/** @brief Reports whether a mode reports its state inverted, as Tasmota's SwitchState. */
[[nodiscard]] constexpr bool inverted_mode(const std::uint8_t mode) {
    return mode == follow_inv || mode == pushbutton_inv || mode == pushbutton_hold_inv ||
           mode == follow_multi_inv || mode == push_hold_multi_inv || mode == push_on_inv ||
           mode == push_ignore_inv;
}

/** @brief Maps a Tasmota POWER_OFF/ON/TOGGLE flag to the Power action. */
[[nodiscard]] constexpr PowerAction power_action(const std::uint32_t flag) {
    return flag == kPowerOff ? PowerAction::off
                             : (flag == kPowerOn ? PowerAction::on : PowerAction::toggle);
}

} // namespace

bool PowerService::switch_state(
    const std::uint8_t instance, const SwitchPolicy& policy, bool& on) const {
    if (!io_.has_switch(instance) || policy.modes == nullptr)
        return false;
    const auto index = instance - 1U;
    on = inverted_mode(policy.modes[index]) != ((switch_last_levels_ & (1U << index)) != 0U);
    return true;
}

void PowerService::poll_switches(
    const std::uint32_t now, const SwitchPolicy& policy, const std::uint16_t debounce) {
    if (io_.switch_count() == 0U || policy.modes == nullptr) {
        return;
    }
    now_ = now;
    io_.probe_switches(now, debounce);
    if (switch_debounce_ != debounce) {
        switch_debounce_ = debounce;
        switch_poll_at_ = now + debounce;
    }
    // Tasmota blocks the switch handler for four seconds after power-on.
    if (!deadline_reached(now, button_enable_at_) || !deadline_reached(now, switch_poll_at_)) {
        return;
    }
    switch_poll_at_ = now + debounce;
    const auto levels = io_.switch_levels();
    const std::uint32_t loops_per_second = 1000U / debounce;
    for (std::size_t index = 0; index < BasicIoDriver::kMaximumSwitches; ++index) {
        if (io_.has_switch(static_cast<std::uint8_t>(index + 1U))) {
            handle_switch(index, (levels & (1U << index)) != 0U, policy, loops_per_second);
        }
    }
}

/** @brief Ports one SwitchHandler iteration with a counter that does not wrap at 8 bits. */
void PowerService::handle_switch(const std::size_t index, const bool high,
    const SwitchPolicy& policy, const std::uint32_t loops_per_second) {
    const auto instance = static_cast<std::uint8_t>(index + 1U);
    const auto bit = 1U << index;
    const std::uint32_t button = high ? kNotPressed : kPressed;
    const std::uint32_t last_state = (switch_last_levels_ & bit) != 0U ? kNotPressed : kPressed;
    const std::uint8_t mode = policy.modes[index];
    const std::uint32_t hold = policy.hold_tenths;
    const std::uint32_t multi_hold_time = loops_per_second * hold / 25U;
    // At least one pass, so a short hold with a long SwitchDebounce still starts the timer.
    const auto hold_ticks =
        static_cast<std::uint16_t>(std::max<std::uint32_t>(1U, loops_per_second * hold / 10U));
    const bool multi_hold = mode == push_hold_multi || mode == push_hold_multi_inv;
    auto& timer = switch_hold_timers_[index];
    auto& flags = switch_press_flags_[index];
    std::uint32_t switchflag = kPowerToggle + 1U;
    std::uint32_t mqtt_action = kPowerNone;
    const auto send_key = [&](const std::uint32_t state) {
        return emit_switch_event(
            {instance, static_cast<std::uint8_t>(state), SwitchEventKind::key});
    };

    if (timer != 0U) {
        --timer;
        if (timer == multi_hold_time && multi_hold) {
            if ((mode == push_hold_multi && last_state == kNotPressed) ||
                (mode == push_hold_multi_inv && last_state == kPressed)) {
                (void)send_key(kPowerIncrement);
            } else if (flags == kFirstPress) {
                (void)send_key(kPowerDelayed);
                mqtt_action = kPowerDelayed;
                timer = 0U;
                flags = 0U;
            }
        }
        if (timer == 0U) {
            switch (mode) {
            case toggle_multi:
                switchflag = kPowerToggle; // Toggle after hold
                break;
            case follow_multi:
                switchflag = button & 1U; // Follow wall switch state after hold
                break;
            case follow_multi_inv:
                switchflag = ~button & 1U; // Follow inverted wall switch state after hold
                break;
            case push_hold_multi:
            case push_hold_multi_inv:
                flags = 0U;
                if (button == (mode == push_hold_multi ? kNotPressed : kPressed)) {
                    timer = static_cast<std::uint16_t>(multi_hold_time);
                    (void)send_key(kPowerIncrement);
                    mqtt_action = kPowerIncrement;
                } else {
                    (void)send_key(kPowerClear);
                    mqtt_action = kPowerClear;
                }
                break;
            default:
                (void)send_key(kPowerHold);
                mqtt_action = kPowerHold;
                break;
            }
        }
    }

    if (button != last_state) {
        switch (mode) {
        case toggle:
        case pushbutton_toggle:
            switchflag = kPowerToggle;
            break;
        case follow:
            switchflag = button & 1U;
            break;
        case follow_inv:
            switchflag = ~button & 1U;
            break;
        case pushbutton:
        case pushbutton_inv:
            if (button == (mode == pushbutton ? kPressed : kNotPressed)) {
                switchflag = kPowerToggle;
            }
            break;
        case pushbutton_hold:
        case pushbutton_hold_inv: {
            const auto press = mode == pushbutton_hold ? kPressed : kNotPressed;
            if (button == press) {
                timer = hold_ticks; // Start timer on button press
            }
            if (button != press && timer != 0U) {
                timer = 0U; // Released before the hold expired: toggle
                switchflag = kPowerToggle;
            }
            break;
        }
        case toggle_multi:
        case follow_multi:
        case follow_multi_inv:
            if (timer != 0U) {
                timer = 0U;
                (void)send_key(kPowerHold);
                mqtt_action = kPowerHold;
            } else {
                // 0.5 s window, at least one pass when SwitchDebounce exceeds 500 ms.
                timer =
                    static_cast<std::uint16_t>(std::max<std::uint32_t>(1U, loops_per_second / 2U));
            }
            break;
        case push_hold_multi:
        case push_hold_multi_inv:
            if (button == (mode == push_hold_multi ? kNotPressed : kPressed)) {
                if (timer != 0U) {
                    flags = flags == kFirstPress ? kSecondPress : 0U;
                    (void)send_key(kPowerInv);
                    mqtt_action = kPowerInv;
                }
            } else if (timer > multi_hold_time) {
                if (flags != kSecondPress) {
                    flags = kFirstPress;
                    switchflag = kPowerToggle;
                } else {
                    (void)send_key(kPower100);
                    mqtt_action = kPower100;
                    flags = 0U;
                }
            } else {
                flags = 0U;
                (void)send_key(kPowerRelease);
                mqtt_action = kPowerRelease;
            }
            timer = hold_ticks;
            break;
        case push_on:
        case push_on_inv:
            if (button == (mode == push_on ? kPressed : kNotPressed)) {
                switchflag = kPowerOn;
            }
            break;
        case push_ignore:
        case push_ignore_inv:
            switch_last_levels_ = high ? switch_last_levels_ | bit : switch_last_levels_ & ~bit;
            (void)emit_switch_event({instance, 0U, SwitchEventKind::report});
            break;
        default:
            break;
        }
        switch_last_levels_ = high ? switch_last_levels_ | bit : switch_last_levels_ & ~bit;
    }

    if (switchflag <= kPowerToggle) {
        if (!policy.detached) {
            if (!send_key(switchflag)) {
                (void)set(instance, power_action(switchflag), PowerOperationSource::switch_input);
            }
        } else {
            mqtt_action = switchflag;
        }
    }
    if (mqtt_action != kPowerNone && policy.detached) {
        (void)emit_switch_event(
            {instance, static_cast<std::uint8_t>(mqtt_action), SwitchEventKind::action});
    }
}

bool PowerService::emit_switch_event(const SwitchEvent& event) {
    return switch_event_handler_ != nullptr && switch_event_handler_(switch_event_context_, event);
}

} // namespace newmota
