/*
  power_service.cpp - relay power and button behavior for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  SetDevicePower/ShowSource reference: Tasmota support_tasmota.ino and support.ino,
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
*/

/** @file power_service.cpp
 * @brief Applies logical relay policies with one optional zero-cross window per operation.
 */

#include "services/power/power_service.h"

#include "newmota/platform/platform.h"
#include "newmota/log.h"
#include "services/settings/state_text.h"
#if USE_ZERO_CROSS
#include "drivers/gpio/zero_cross.h"
#endif
#if USE_PWM || USE_WS2812
#include "services/light/light_service.h"
#endif

#include <cstddef>
#include <cstdint>

namespace newmota {

static_assert(BasicIoDriver::kMaximumRelays == kMaximumPowerOutputs);

/** @brief Initializes safe pins and applies Tasmota-compatible startup state. */
BasicIoInitializationResult PowerService::initialize(const Hardware& hardware,
    const std::uint32_t now, const PowerSettings& settings, const bool save_state,
    const bool power_on_reset) {
    settings_ = &settings;
    save_state_ = save_state;
    state_mask_ = 0U;
    state_revision_ = 0U;
    pulse_deadlines_ = {};
    pulse_active_mask_ = 0U;
    blink_mask_ = 0U;
    blink_deadline_ = 0U;
    blink_transitions_ = 0U;
    blink_output_on_ = false;
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    button_deadlines_ = {};
    button_press_counts_ = {};
    button_press_started_ = {};
    button_hold_armed_ = button_hold_sent_ = 0U;
    button_enable_at_ = now + kStartupGuardMilliseconds;
    button_poll_at_ = button_enable_at_;
    button_debounce_ = 0U;
#endif
    now_ = now;
    always_on_ = false;
#if USE_POWER_PROVENANCE
    last_source_ = last_force_source_ = PowerOperationSource::unspecified;
#endif
#if USE_POWER_TARGETS
    last_nonzero_targets_ = target_update_depth_ = 0;
    TargetUpdate target_update(this);
#endif
    auto result = io_.initialize(hardware);
#if USE_SWITCHES
    // As SwitchInit, the boot level is the last state, so startup never switches Power.
    switch_last_levels_ = io_.switch_levels();
    switch_hold_timers_ = {};
    switch_press_flags_ = {};
    switch_poll_at_ = button_enable_at_;
    switch_debounce_ = 0U;
#endif

    std::uint32_t startup_state = 0U;
    if (settings.power_on_state == 4U) {
        startup_state = UINT32_MAX;
        always_on_ = true;
    } else if (!power_on_reset) {
        startup_state = save_state ? settings.saved_state : 0U;
    } else {
        switch (settings.power_on_state) {
        case 1U:
            startup_state = UINT32_MAX;
            break;
        case 2U:
            startup_state = save_state ? ~settings.saved_state : 0U;
            break;
        case 3U:
            startup_state = save_state ? settings.saved_state : 0U;
            break;
        case 0U:
        case 4U:
        case 5U:
        default:
            break;
        }
    }
    /** Match SetPowerOnState call eligibility, including all-OFF startup and SaveState gates. */
    if (settings.power_on_state == 4U ||
        (power_on_reset && (settings.power_on_state == 0U || settings.power_on_state == 1U ||
                               settings.power_on_state == 5U)) ||
        save_state)
        record_source(PowerOperationSource::restart);
    startup_state &= relay_mask();

    if (settings.interlock_enabled) {
        for (std::size_t index = 0; index < kMaximumInterlockGroups; ++index) {
            const auto group = interlock_group(index);
            const auto group_state = startup_state & group;
            if ((group_state & (group_state - 1U)) != 0U) {
                startup_state &= ~group;
            }
        }
    }

    for (std::size_t index = 0; index < kMaximumPowerOutputs; ++index) {
        if ((startup_state & (1U << index)) == 0U) {
#if USE_TUYA_MCU
            // GPIO relays start off; a relay without a pin keeps its own state, so it also
            // gets the off states, as Tasmota's SetDevicePower writes the whole startup mask.
            if ((io_.external_relays() & (1U << index)) != 0U &&
                io_.write_relay(static_cast<std::uint8_t>(index + 1U), false) !=
                    platform::GpioResult::success) {
                ++result.failure_count;
            }
#endif
            continue;
        }
        if (write_logical(static_cast<std::uint8_t>(index + 1U), true) ==
            PowerChangeResult::failed) {
            ++result.failure_count;
        }
    }
    for (std::size_t index = 0; index < kMaximumPowerOutputs; ++index) {
        arm_pulse(static_cast<std::uint8_t>(index), now);
    }
    return result;
}

void PowerService::record_source(PowerOperationSource source) {
#if USE_POWER_PROVENANCE
    last_source_ = source;
    const char* name = nullptr;
    switch (source) {
    case PowerOperationSource::mqtt:
        name = "MQTT";
        break;
    case PowerOperationSource::restart:
        name = "Restart";
        break;
    case PowerOperationSource::button:
        name = "Button";
        break;
    case PowerOperationSource::switch_input:
        name = "Switch";
        break;
    case PowerOperationSource::serial:
        name = "Serial";
        break;
    case PowerOperationSource::web_gui:
        name = "WebGui";
        break;
    case PowerOperationSource::web_command:
        name = "WebCommand";
        break;
    case PowerOperationSource::web_console:
        name = "WebConsole";
        break;
    case PowerOperationSource::pulse_timer:
        name = "PulseTimer";
        break;
    case PowerOperationSource::timer:
        name = "Timer";
        break;
    case PowerOperationSource::max_power:
        name = "MaxPower";
        break;
    case PowerOperationSource::max_energy:
        name = "MaxEnergy";
        break;
    case PowerOperationSource::overtemperature:
        name = "Overtemp";
        break;
    case PowerOperationSource::light:
        name = "Light";
        break;
    case PowerOperationSource::berry:
        name = "Berry";
        break;
    default:
        break;
    }
    if (name)
        AddLog(LogLevel::debug, "SRC: %s", name);
#else
    (void)source;
#endif
}

/** @brief Applies one logical or blinking action to a configured relay. */
PowerChangeResult PowerService::set(
    const std::uint8_t instance, const PowerAction action, PowerOperationSource source) {
#if USE_PWM || USE_WS2812
    const auto endpoint = light_ ? light_->endpoints().get(instance) : PowerEndpoint{};
    const bool light_endpoint = endpoint.owner == PowerOwner::light;
#else
    constexpr bool light_endpoint = false;
#endif
    bool configured{};
    const bool present = state(instance, configured);
#if USE_POWER_TARGETS
    TargetUpdate target_update(this, present && action != PowerAction::blink &&
                                         (action != PowerAction::blink_stop || blinking(instance)));
#endif
    if (!light_endpoint && present)
        record_source(source);
#if USE_ENERGY_POWER_DELTA || USE_POWER_TARGETS
    bool configured_on{};
    if (state(instance, configured_on))
        ++operation_revision_;
#endif
#if USE_ZERO_CROSS
    zero_cross::WriteGroup timing_group;
#endif
#if USE_PWM || USE_WS2812
    if (light_endpoint) {
        const auto result = light_->set(endpoint.instance, action, source);
#if USE_BERRY_DRIVERS
        if (operation_handler_ &&
            (result == PowerChangeResult::changed || result == PowerChangeResult::unchanged))
            operation_handler_(operation_context_);
#endif
        log_runtime_result(instance, result, source);
        return result;
    }
#endif
    if (action == PowerAction::blink) {
        return start_blink(instance);
    }
    if (action == PowerAction::blink_stop) {
        return stop_blink(instance);
    }
    const auto result = set_logical(instance, action);
    log_runtime_result(instance, result, source);
    return result;
}

/** @brief Reports successful autonomous operations independently of MQTT and command replies. */
void PowerService::log_runtime_result(const std::uint8_t instance, const PowerChangeResult result,
    const PowerOperationSource source) const {
    if ((source != PowerOperationSource::button && source != PowerOperationSource::switch_input &&
            source != PowerOperationSource::timer && source != PowerOperationSource::pulse_timer) ||
        (result != PowerChangeResult::changed && result != PowerChangeResult::unchanged)) {
        return;
    }
    bool on = false;
    if (!state(instance, on)) {
        return;
    }
    const auto word = state_text(on);
    if (instance == 1U && endpoint_count() == 1U) {
        AddLog(LogLevel::info, "RSL: POWER = %.*s", static_cast<int>(word.size()), word.data());
    } else {
        AddLog(LogLevel::info, "RSL: POWER%u = %.*s", unsigned(instance),
            static_cast<int>(word.size()), word.data());
    }
}

/** @brief Applies one ordinary logical action to every configured relay. */
PowerChangeResult PowerService::set_all(const PowerAction action, PowerOperationSource source) {
#if USE_POWER_TARGETS
    TargetUpdate target_update(this, action != PowerAction::blink);
#endif
#if USE_ZERO_CROSS
    zero_cross::WriteGroup timing_group;
#endif
    bool changed = false;
    bool available = false;
    for (std::size_t index = 0; index < kMaximumPowerOutputs; ++index) {
        const auto instance = static_cast<std::uint8_t>(index + 1U);
        bool on = false;
        if (!state(instance, on))
            continue;
        available = true;
        const auto result = set(instance, action, source);
        if (result == PowerChangeResult::failed) {
            return result;
        }
        changed = changed || result == PowerChangeResult::changed;
    }
    if (!available) {
        return PowerChangeResult::unavailable;
    }
    return changed ? PowerChangeResult::changed : PowerChangeResult::unchanged;
}

/** @brief Reads one configured relay's retained logical state. */
bool PowerService::state(const std::uint8_t instance, bool& on) const {
#if USE_PWM || USE_WS2812
    if (light_ != nullptr) {
        const auto endpoint = light_->endpoints().get(instance);
        if (endpoint.owner == PowerOwner::light)
            return light_->state(endpoint.instance, on);
    }
#endif
    if (!io_.has_relay(instance)) {
        return false;
    }
    on = (state_mask_ & (1U << (instance - 1U))) != 0U;
    return true;
}

/** @brief Reports whether a one-based relay instance is configured. */
bool PowerService::has_relay(const std::uint8_t instance) const {
    return io_.has_relay(instance);
}

/** @brief Enables immutable all-on operation or releases that restriction. */
PowerChangeResult PowerService::set_always_on(const bool enabled) {
#if USE_ZERO_CROSS
    zero_cross::WriteGroup timing_group;
#endif
    if (!enabled) {
        always_on_ = false;
        return PowerChangeResult::unchanged;
    }
#if USE_POWER_TARGETS
    TargetUpdate target_update(this);
#endif
    always_on_ = false;
    bool changed = false;
    bool available = false;
    for (std::size_t relay = 0; relay < kMaximumPowerOutputs; ++relay) {
        const auto instance = static_cast<std::uint8_t>(relay + 1U);
        if (!io_.has_relay(instance)) {
            continue;
        }
        available = true;
        record_source(PowerOperationSource::unspecified);
        const auto mask = 1U << relay;
        bool grouped = false;
        if (settings_->interlock_enabled) {
            for (std::size_t group_index = 0; group_index < kMaximumInterlockGroups;
                ++group_index) {
                grouped = grouped || (interlock_group(group_index) & mask) != 0U;
            }
        }
        const auto result = set_logical(instance, grouped ? PowerAction::off : PowerAction::on);
        if (result == PowerChangeResult::failed) {
            always_on_ = true;
            return result;
        }
        changed = changed || result == PowerChangeResult::changed;
    }
    always_on_ = true;
    if (!available) {
        return PowerChangeResult::unavailable;
    }
    return changed ? PowerChangeResult::changed : PowerChangeResult::unchanged;
}

/** @brief Restarts one pulse timer after its persistent duration changes. */
void PowerService::update_pulse_time(const std::uint8_t index, const std::uint32_t now) {
    arm_pulse(index, now);
}

/** @brief Restarts an active blink cadence using the new half-period. */
void PowerService::update_blink_time(const std::uint32_t now) {
    if (blink_mask_ != 0U) {
        blink_deadline_ = now + static_cast<std::uint32_t>(settings_->blink_time) * 100U;
    }
}

/** @brief Applies a changed finite or continuous count to active blinking. */
void PowerService::update_blink_count() {
    if (blink_mask_ == 0U) {
        return;
    }
    blink_transitions_ = settings_->blink_count == 0U
                             ? 64000U
                             : static_cast<std::uint16_t>(settings_->blink_count * 2U);
}

/** @brief Turns off conflicting live states after interlock changes. */
PowerChangeResult PowerService::apply_interlock() {
#if USE_ZERO_CROSS
    zero_cross::WriteGroup timing_group;
#endif
    if (!settings_->interlock_enabled) {
        return PowerChangeResult::unchanged;
    }
    record_source(PowerOperationSource::unspecified);
#if USE_POWER_TARGETS
    TargetUpdate target_update(this);
#endif

    bool changed = false;
    for (std::size_t group_index = 0; group_index < kMaximumInterlockGroups; ++group_index) {
        const auto group = interlock_group(group_index);
        const auto active = (state_mask_ | blink_mask_) & group;
        if ((active & (active - 1U)) == 0U) {
            continue;
        }
        for (std::size_t relay = 0; relay < kMaximumPowerOutputs; ++relay) {
            const auto mask = 1U << relay;
            if ((group & mask) == 0U) {
                continue;
            }
            const auto instance = static_cast<std::uint8_t>(relay + 1U);
            if (stop_blink(instance) == PowerChangeResult::failed) {
                return PowerChangeResult::failed;
            }
            pulse_active_mask_ &= ~mask;
            const auto result = write_logical(instance, false);
            if (result == PowerChangeResult::failed) {
                return result;
            }
            changed = changed || result == PowerChangeResult::changed;
        }
    }
    return changed ? PowerChangeResult::changed : PowerChangeResult::unchanged;
}

/** @brief Returns one active pulse deadline in Tasmota command units. */
std::uint16_t PowerService::remaining_pulse_time(
    const std::uint8_t index, const std::uint32_t now) const {
    if (index >= kMaximumPowerOutputs || (pulse_active_mask_ & (1U << index)) == 0U ||
        deadline_reached(now, pulse_deadlines_[index])) {
        return 0U;
    }
    return encode_pulse_remaining(pulse_deadlines_[index] - now);
}

/** @brief Reports whether one configured relay is in the blink mask. */
bool PowerService::blinking(const std::uint8_t instance) const {
    return instance != 0U && instance <= kMaximumPowerOutputs &&
           (blink_mask_ & (1U << (instance - 1U))) != 0U;
}

#ifndef NEWMOTA_IMAGE_SAFEBOOT
/** @brief Probes filtered inputs and dispatches policy at the configured handler cadence. */
void PowerService::poll_buttons(
    const std::uint32_t now, const ButtonPolicy policy, const std::uint16_t debounce) {
    if (io_.button_count() == 0U) {
        return;
    }
    now_ = now;
    button_policy_ = policy;
    io_.probe_buttons(now, debounce);
    if (button_debounce_ != debounce) {
        button_debounce_ = debounce;
        button_poll_at_ = now + debounce;
    }
    if (!deadline_reached(now, button_enable_at_)) {
        // Track filtered levels even between handler ticks; guarded presses must not queue.
        io_.poll_buttons(button_edge, this);
        button_press_counts_ = {};
        button_hold_armed_ = button_hold_sent_ = 0U;
        return;
    }
    if (!deadline_reached(now, button_poll_at_)) {
        return;
    }
    button_poll_at_ = now + debounce;
    io_.poll_buttons(button_edge, this);

    for (std::size_t index = 0; index < button_press_counts_.size(); ++index) {
        const auto bit = 1U << index;
        if (!policy.immediate && (button_hold_armed_ & bit) != 0U &&
            (button_hold_sent_ & bit) == 0U &&
            now - button_press_started_[index] >=
                static_cast<std::uint32_t>(policy.hold_tenths) * 100U) {
            button_hold_sent_ |= bit;
            button_press_counts_[index] = 0U;
            const auto instance = static_cast<std::uint8_t>(index + 1U);
            (void)emit_button_event({instance, instance, 3U, ButtonAction::hold, policy.decoupled});
        }
        const auto count = button_press_counts_[index];
        if (count == 0U || !deadline_reached(now, button_deadlines_[index]) ||
            io_.button_pressed(static_cast<std::uint8_t>(index + 1U))) {
            continue;
        }
        button_press_counts_[index] = 0U;
        execute_button_sequence(static_cast<std::uint8_t>(index + 1U), count);
    }
}
#endif

/** @brief Expires relay pulses and advances the shared blink cadence. */
void PowerService::poll_timers(const std::uint32_t now) {
#if USE_ZERO_CROSS
    zero_cross::WriteGroup timing_group;
#endif
    now_ = now;
    for (std::size_t index = 0; index < kMaximumPowerOutputs; ++index) {
        const auto mask = 1U << index;
        if ((pulse_active_mask_ & mask) == 0U || !deadline_reached(now, pulse_deadlines_[index])) {
            continue;
        }
        pulse_active_mask_ &= ~mask;
        record_source(PowerOperationSource::pulse_timer);
#if USE_POWER_TARGETS
        TargetUpdate target_update(this);
#endif
        const auto instance = static_cast<std::uint8_t>(index + 1U);
        const auto result = set_logical(
            instance, settings_->power_on_state == 5U ? PowerAction::on : PowerAction::off);
        log_runtime_result(instance, result, PowerOperationSource::pulse_timer);
    }

    if (blink_mask_ == 0U || !deadline_reached(now, blink_deadline_)) {
        return;
    }
    blink_deadline_ = now + static_cast<std::uint32_t>(settings_->blink_time) * 100U;
    record_source(PowerOperationSource::unspecified);
    if (blink_transitions_ != 0U) {
        --blink_transitions_;
    }
    if (blink_transitions_ == 0U) {
        const auto active = blink_mask_;
        for (std::size_t index = 0; index < kMaximumPowerOutputs; ++index) {
            if ((active & (1U << index)) != 0U) {
                (void)stop_blink(static_cast<std::uint8_t>(index + 1U));
            }
        }
        return;
    }

#if USE_ENERGY_POWER_DELTA || USE_POWER_TARGETS
    ++operation_revision_; // Blink writes bypass the logical state mask.
#endif
    blink_output_on_ = !blink_output_on_;
#if USE_POWER_TARGETS
    /** Source blink SetDevicePower remembers its physical mask without changing logical Power. */
    remember_targets((target_intent_mask() & ~blink_mask_) | (blink_output_on_ ? blink_mask_ : 0));
#endif
    for (std::size_t index = 0; index < kMaximumPowerOutputs; ++index) {
        if ((blink_mask_ & (1U << index)) != 0U) {
            (void)io_.write_relay(static_cast<std::uint8_t>(index + 1U), blink_output_on_);
        }
    }
}

#ifndef NEWMOTA_IMAGE_SAFEBOOT
/** @brief Receives one driver button edge through the allocation-free callback. */
void PowerService::button_edge(void* const context, const ButtonEdge& edge) {
    static_cast<PowerService*>(context)->handle_button_edge(edge);
}

/** @brief Records one button edge against the current polling policy. */
void PowerService::handle_button_edge(const ButtonEdge& edge) {
    if (!deadline_reached(now_, button_enable_at_)) {
        return;
    }
    const std::size_t index = edge.instance - 1U;
#if USE_BERRY_DRIVERS
    if (button_level_handler_)
        button_level_handler_(
            button_level_context_, edge.instance, edge.pressed, button_press_counts_[index]);
#endif
    const auto bit = 1U << index;
    if (!edge.pressed) {
        const bool clear = (button_hold_armed_ & bit) != 0U &&
                           ((button_hold_sent_ & bit) != 0U || button_policy_.immediate);
        button_hold_armed_ &= ~bit;
        button_hold_sent_ &= ~bit;
        if (clear && button_policy_.decoupled)
            (void)emit_button_event({edge.instance, edge.instance, 15U, ButtonAction::clear, true});
        return;
    }
    button_press_started_[index] = now_;
    button_hold_armed_ |= bit;
    button_hold_sent_ &= ~bit;
    if (button_policy_.immediate) {
        AddLog(LogLevel::debug, "BTN: Button%u immediate", unsigned(edge.instance));
        const bool consumed = emit_button_event({edge.instance, edge.instance,
            static_cast<std::uint8_t>(button_policy_.decoupled ? 10U : 2U), ButtonAction::single,
            button_policy_.decoupled, true, true, true});
        if (!button_policy_.decoupled && !consumed)
            (void)set(edge.instance, PowerAction::toggle, PowerOperationSource::button);
        return;
    }
    auto& count = button_press_counts_[index];
    if (count < 6U) {
        ++count;
    }
    AddLog(
        LogLevel::debug, "BTN: Button%u multi-press %u", unsigned(edge.instance), unsigned(count));
    button_deadlines_[index] = now_ + kMultiPressWindowMilliseconds;
}

/** @brief Routes single presses and Button1 multi-presses through logical Power endpoints. */
void PowerService::execute_button_sequence(
    const std::uint8_t instance, const std::uint8_t press_count) {
    if (press_count == 0U || press_count >= 6U) {
        return;
    }
    auto mapped_count = press_count;
    const bool single = press_count == (button_policy_.swap_single_double ? 2U : 1U);
    bool on = false;
    if (button_policy_.swap_single_double && instance == 1U && io_.button_count() == 1U &&
        endpoint_count() == 2U && state(1U, on) && state(2U, on) && press_count <= 2U) {
        mapped_count = press_count == 1U ? 2U : 1U;
    }
    const auto endpoint = static_cast<std::uint8_t>(instance + mapped_count - 1U);
    const bool decoupled = button_policy_.decoupled;
    const auto key_instance = static_cast<std::uint8_t>(single && !decoupled ? endpoint : instance);
    const bool toggle_key = !decoupled && (single || mapped_count == 1U);
    const auto key_state = static_cast<std::uint8_t>(toggle_key ? 2U : mapped_count + 9U);
    const bool consumed = emit_button_event(
        {instance, key_instance, key_state, static_cast<ButtonAction>(mapped_count), decoupled,
            decoupled || single || mapped_count > 1U});
    if (decoupled || (single && consumed))
        return;
    if (single && mapped_count > 1U) {
        // SO11 can try a toggle key first, then the ordinary multi-key on local fallback.
        (void)emit_button_event({instance, instance, static_cast<std::uint8_t>(mapped_count + 9U),
            static_cast<ButtonAction>(mapped_count), false, true, false});
    }
    if (instance != 1U && mapped_count > 1U) {
        return; // Tasmota exposes these as key events; only Button1 maps them to Power outputs.
    }
    (void)set(endpoint, PowerAction::toggle, PowerOperationSource::button);
}

bool PowerService::emit_button_event(const ButtonEvent& event) {
    return button_event_handler_ != nullptr && button_event_handler_(button_event_context_, event);
}
#endif

/** @brief Applies a logical action, interlock, and pulse rearming. */
PowerChangeResult PowerService::set_logical(const std::uint8_t instance, const PowerAction action) {
    if (!io_.has_relay(instance) || action == PowerAction::blink ||
        action == PowerAction::blink_stop) {
        return PowerChangeResult::unavailable;
    }

#if USE_ENERGY_POWER_DELTA || USE_POWER_TARGETS
    ++operation_revision_; // Includes button/pulse/always-on paths that bypass set().
#endif
    const auto mask = 1U << (instance - 1U);
    if (stop_blink(instance) == PowerChangeResult::failed) {
        return PowerChangeResult::failed;
    }
    pulse_active_mask_ &= ~mask;

    const bool previous = (state_mask_ & mask) != 0U;
    bool next = previous;
    switch (action) {
    case PowerAction::off:
        next = false;
        break;
    case PowerAction::on:
        next = true;
        break;
    case PowerAction::toggle:
        next = !previous;
        break;
    case PowerAction::blink:
    case PowerAction::blink_stop:
        break;
    }
    if (always_on_) {
        next = true;
    }

    bool changed = false;
    if (next && settings_->interlock_enabled) {
        for (std::size_t group_index = 0; group_index < kMaximumInterlockGroups; ++group_index) {
            const auto group = interlock_group(group_index);
            if ((group & mask) == 0U) {
                continue;
            }
            const auto conflicts = state_mask_ & group & ~mask;
            for (std::size_t relay = 0; relay < kMaximumPowerOutputs; ++relay) {
                const auto conflict_mask = 1U << relay;
                if ((conflicts & conflict_mask) == 0U) {
                    continue;
                }
                const auto conflict = static_cast<std::uint8_t>(relay + 1U);
                if (stop_blink(conflict) == PowerChangeResult::failed) {
                    return PowerChangeResult::failed;
                }
                pulse_active_mask_ &= ~conflict_mask;
                const auto result = write_logical(conflict, false);
                if (result == PowerChangeResult::failed) {
                    return result;
                }
                changed = changed || result == PowerChangeResult::changed;
            }
            if (conflicts != 0U) {
#if USE_ZERO_CROSS
                zero_cross::next_window();
#endif
                platform::sleep_for_milliseconds(kInterlockDelayMilliseconds);
            }
            break;
        }
    }

    const auto result = write_logical(instance, next);
    if (result == PowerChangeResult::failed) {
        return result;
    }
    changed = changed || result == PowerChangeResult::changed;
    arm_pulse(static_cast<std::uint8_t>(instance - 1U), now_);
    return changed ? PowerChangeResult::changed : PowerChangeResult::unchanged;
}

/** @brief Writes and records one relay state without policy recursion. */
PowerChangeResult PowerService::write_logical(const std::uint8_t instance, const bool on) {
    if (!io_.has_relay(instance)) {
        return PowerChangeResult::unavailable;
    }
#if USE_ENERGY_POWER_DELTA || USE_POWER_TARGETS
    ++operation_revision_;
#endif
    const auto mask = 1U << (instance - 1U);
    const bool previous = (state_mask_ & mask) != 0U;
    if (previous == on) {
#if USE_BERRY_DRIVERS
        if (operation_handler_)
            operation_handler_(operation_context_);
#endif
        return PowerChangeResult::unchanged;
    }
    if (io_.write_relay(instance, on) != platform::GpioResult::success) {
        return PowerChangeResult::failed;
    }
    state_mask_ = on ? state_mask_ | mask : state_mask_ & ~mask;
    ++state_revision_;
    if (state_revision_ == 0U) {
        state_revision_ = 1U;
    }
#if USE_BERRY_DRIVERS
    if (operation_handler_)
        operation_handler_(operation_context_);
#endif
    return PowerChangeResult::changed;
}

/** @brief Stops one blinking relay and restores its logical output. */
PowerChangeResult PowerService::stop_blink(const std::uint8_t instance) {
#if USE_POWER_TARGETS
    TargetUpdate target_update(this, blinking(instance));
#endif
    if (!io_.has_relay(instance)) {
        return PowerChangeResult::unavailable;
    }
    const auto mask = 1U << (instance - 1U);
    if ((blink_mask_ & mask) == 0U) {
        return PowerChangeResult::unchanged;
    }
#if USE_ENERGY_POWER_DELTA || USE_POWER_TARGETS
    ++operation_revision_;
#endif
    const bool restore_on = (state_mask_ & mask) != 0U;
    if (io_.write_relay(instance, restore_on) != platform::GpioResult::success) {
        return PowerChangeResult::failed;
    }
    blink_mask_ &= ~mask;
    if (blink_mask_ == 0U) {
        blink_transitions_ = 0U;
    }
    return PowerChangeResult::changed;
}

/** @brief Starts or refreshes Tasmota's shared relay blink sequence. */
PowerChangeResult PowerService::start_blink(const std::uint8_t instance) {
    if (!io_.has_relay(instance)) {
        return PowerChangeResult::unavailable;
    }
    if (always_on_) {
        return PowerChangeResult::unchanged;
    }

    const auto mask = 1U << (instance - 1U);
    if (settings_->interlock_enabled) {
        for (std::size_t group_index = 0; group_index < kMaximumInterlockGroups; ++group_index) {
            const auto group = interlock_group(group_index);
            if ((group & mask) == 0U) {
                continue;
            }
            const auto conflicts = (state_mask_ | blink_mask_) & group & ~mask;
            for (std::size_t relay = 0; relay < kMaximumPowerOutputs; ++relay) {
                const auto conflict_mask = 1U << relay;
                if ((conflicts & conflict_mask) == 0U) {
                    continue;
                }
                const auto conflict = static_cast<std::uint8_t>(relay + 1U);
                if (stop_blink(conflict) == PowerChangeResult::failed) {
                    return PowerChangeResult::failed;
                }
                pulse_active_mask_ &= ~conflict_mask;
                if (write_logical(conflict, false) == PowerChangeResult::failed) {
                    return PowerChangeResult::failed;
                }
            }
            if (conflicts != 0U) {
#if USE_ZERO_CROSS
                zero_cross::next_window();
#endif
                platform::sleep_for_milliseconds(kInterlockDelayMilliseconds);
            }
            break;
        }
    }
    if ((blink_mask_ & mask) == 0U) {
        blink_output_on_ = (state_mask_ & mask) != 0U;
    }
    pulse_active_mask_ &= ~mask;
    blink_mask_ |= mask;
    blink_deadline_ = now_ + 100U;
    blink_transitions_ = static_cast<std::uint16_t>(
        (settings_->blink_count == 0U ? 64000U : settings_->blink_count * 2U) + 1U);
    return PowerChangeResult::changed;
}

/** @brief Arms one pulse when its configured active state is present. */
void PowerService::arm_pulse(const std::uint8_t index, const std::uint32_t now) {
    if (index >= kMaximumPowerOutputs || !io_.has_relay(static_cast<std::uint8_t>(index + 1U))) {
        return;
    }
    const auto mask = 1U << index;
    pulse_active_mask_ &= ~mask;
    const auto value = settings_->pulse_times[index];
    const bool on = (state_mask_ & mask) != 0U;
    const bool timed_state = settings_->power_on_state == 5U ? !on : on;
    if (value == 0U || !timed_state || settings_->power_on_state == 4U || blinking(index + 1U)) {
        return;
    }
    pulse_deadlines_[index] = now + pulse_milliseconds(value);
    pulse_active_mask_ |= mask;
}

/** @brief Resolves explicit groups or the default all-relay group. */
std::uint32_t PowerService::interlock_group(const std::size_t index) const {
    bool explicit_groups = false;
    for (const auto group : settings_->interlock_groups) {
        explicit_groups = explicit_groups || group != 0U;
    }
    if (!explicit_groups) {
        return index == 0U ? relay_mask() : 0U;
    }
    return index < settings_->interlock_groups.size()
               ? settings_->interlock_groups[index] & relay_mask()
               : 0U;
}

/** @brief Builds the bit mask of successfully configured relay instances. */
std::uint32_t PowerService::relay_mask() const {
    std::uint32_t mask = 0U;
    for (std::size_t index = 0; index < kMaximumPowerOutputs; ++index) {
        if (io_.has_relay(static_cast<std::uint8_t>(index + 1U))) {
            mask |= 1U << index;
        }
    }
    return mask;
}

/** @brief Encodes remaining milliseconds using Tasmota PulseTime units. */
std::uint16_t PowerService::encode_pulse_remaining(const std::uint32_t milliseconds) {
    return milliseconds > 11100U ? static_cast<std::uint16_t>(milliseconds / 1000U + 100U)
                                 : static_cast<std::uint16_t>(milliseconds / 100U);
}

/** @brief Converts Tasmota PulseTime units into a millisecond duration. */
std::uint32_t PowerService::pulse_milliseconds(const std::uint16_t value) {
    return value > 111U ? static_cast<std::uint32_t>(value - 100U) * 1000U
                        : static_cast<std::uint32_t>(value) * 100U;
}

/** @brief Reports whether one rollover-safe millisecond deadline is due. */
bool PowerService::deadline_reached(const std::uint32_t now, const std::uint32_t deadline) {
    return static_cast<std::int32_t>(now - deadline) >= 0;
}

} // namespace newmota

#if USE_PWM || USE_WS2812
namespace newmota {
std::uint32_t PowerService::state_mask() const {
    auto value = state_mask_;
#if USE_PWM || USE_WS2812
    value |= light_ == nullptr ? 0U : light_->state_mask();
#endif
    return value;
}

std::uint32_t PowerService::state_revision() const {
    auto value = state_revision_;
#if USE_PWM || USE_WS2812
    value += light_ == nullptr ? 0U : light_->revision();
#endif
    return value;
}
} // namespace newmota
#endif
