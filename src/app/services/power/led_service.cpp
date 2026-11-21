/*
  led_service.cpp - Tasmota-compatible status and link LED behavior

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Source reference: Tasmota support_tasmota.ino (SetLedPowerIdx, SetLedPower, SetLedLink,
  Every250mSeconds) and support_command.ino (CmndLedPower, CmndLedState),
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
*/

#include "services/power/led_service.h"

#include "drivers/io/basic_io.h"
#if USE_PWM
#include "services/light/light_color.h"
#endif

namespace newmota {
namespace {
constexpr std::uint8_t kMaximumLeds = 4U;
constexpr std::uint8_t kManualOn = 0x08U;
constexpr std::uint8_t kMqttActivity = 0x06U;
constexpr std::uint8_t kMqttReceive = 0x02U;
constexpr std::uint8_t kMqttPublish = 0x04U;
constexpr std::uint8_t kPowerIndication = 0x01U;
/** Blinks above this value mark a network or MQTT problem rather than activity. */
constexpr std::uint16_t kProblemBlinks = 200U;

/** @brief Adds MQTT activity blinks without entering the problem range. */
std::uint16_t add_activity(const std::uint16_t blinks, const std::uint32_t events) {
    if (blinks > kProblemBlinks)
        return blinks;
    const auto total = static_cast<std::uint32_t>(blinks) + events;
    return static_cast<std::uint16_t>(total > kProblemBlinks ? kProblemBlinks : total);
}
} // namespace

bool LedService::has_link_led() const {
    return io_.has_link_led();
}

/** @brief SetLedPowerIdx: drives one zero-based LED and records its state. */
void LedService::set_led(const std::uint8_t index, const bool on) {
    if (index >= kMaximumLeds || !io_.has_led(static_cast<std::uint8_t>(index + 1U)))
        return;
    const auto mask = static_cast<std::uint8_t>(1U << index);
    led_power_ = on ? static_cast<std::uint8_t>(led_power_ | mask)
                    : static_cast<std::uint8_t>(led_power_ & ~mask);
#if USE_PWM
    if (pwm_[index].valid()) {
        // Like Tasmota's ledGamma10, PWM levels follow the shared Light gamma curve.
        const auto level = static_cast<std::uint16_t>((on ? pwm_on_ : pwm_off_) * 257U);
        const platform::PwmDutyUpdate update{pwm_[index], light_color::gamma16(level)};
        platform::PwmOperationResult result{};
        if (platform::pwm_write_batch(&update, 1U, &result) == platform::PwmResult::success &&
            result.result == platform::PwmResult::success)
            return;
    }
#endif
    (void)io_.write_led(static_cast<std::uint8_t>(index + 1U), on);
}

void LedService::refresh() {
    for (std::uint8_t index = 0U; index < kMaximumLeds; ++index)
        set_led(index, (led_power_ & (1U << index)) != 0U);
}

void LedService::sync_pwm(const LedPolicy& policy) {
    if (policy.pwm_mask == pwm_mask_ && policy.pwm_on == pwm_on_ && policy.pwm_off == pwm_off_)
        return;
    pwm_mask_ = policy.pwm_mask;
    pwm_on_ = policy.pwm_on;
    pwm_off_ = policy.pwm_off;
#if USE_PWM
    for (std::uint8_t index = 0U; index < kMaximumLeds; ++index) {
        const auto instance = static_cast<std::uint8_t>(index + 1U);
        const bool wanted = (pwm_mask_ & (1U << index)) != 0U && io_.has_led(instance);
        if (!wanted && pwm_[index].valid()) {
            // Release returns the pad to a safe GPIO output; refresh rewrites its state.
            (void)platform::pwm_release(pwm_[index]);
            pwm_[index] = {};
        } else if (wanted && !pwm_[index].valid()) {
            platform::PwmRequest request{};
            request.logical_output = {platform::PwmOutputOwner::status_led, instance};
            request.pin = io_.led_pin(instance);
            request.frequency_hz = policy.carrier_hz;
            request.max_error_ppm = platform::kMaximumPwmErrorPpm;
            request.min_resolution_bits = 8U;
            request.inverted = io_.led_inverted(instance);
            platform::PwmPlanToken token{};
            platform::PwmRequestResult allocation{};
            // A pad without a PWM route keeps on/off control.
            if (platform::pwm_prepare(&request, 1U, token, &allocation) ==
                    platform::PwmResult::success &&
                allocation.result == platform::PwmResult::success &&
                platform::pwm_activate(token, &allocation, 1U) == platform::PwmResult::success &&
                allocation.result == platform::PwmResult::success)
                pwm_[index] = allocation.handle;
        }
    }
#endif
    refresh();
}

void LedService::set_all(const bool on) {
    for (std::uint8_t index = 0U; index < kMaximumLeds; ++index)
        set_led(index, on);
}

/**
 * @brief SetLedPower: without a link LED, LED2 (or LED1 alone) shows power; with a link
 * LED, LED n mirrors Power n.
 */
void LedService::set_power_leds(const bool on, const std::uint32_t power) {
    if (!io_.has_link_led()) {
        set_led(io_.has_led(2U) ? 1U : 0U, on);
        return;
    }
    for (std::uint8_t index = 0U; index < kMaximumLeds; ++index)
        set_led(index, (power & (1UL << index)) != 0U);
}

/** @brief SetLedLink: the link LED, or LED1 when no link LED is configured. */
void LedService::set_link(const bool on) {
    if (io_.has_link_led())
        (void)io_.write_link_led(on);
    else
        set_led(0U, on);
}

void LedService::start(const LedPolicy& policy) {
    blinks_ = kProblemBlinks + 1U;
    blink_speed_ = 1U;
    blink_state_ = false;
    counters_seeded_ = false;
    pwm_mask_ = 0U;
    pwm_on_ = 255U;
    pwm_off_ = 0U;
    sync_pwm(policy);
    const bool manual_on = (policy.state & kManualOn) != 0U;
    set_power_leds(manual_on, 0U);
    set_link(manual_on);
}

void LedService::tick(const LedInputs& inputs, const LedPolicy& policy) {
    sync_pwm(policy);
    std::uint8_t interval = 1U;
    if (!policy.no_problem_blink && (inputs.network_down || inputs.mqtt_down)) {
        if (inputs.mqtt_down)
            interval = 7U; // MQTT problem: blink every 2 seconds.
        if (inputs.network_down)
            interval = 3U; // Network problem: blink every second.
        blinks_ = kProblemBlinks + 1U;
    }
    if (counters_seeded_) {
        if ((policy.state & kMqttPublish) != 0U)
            blinks_ = add_activity(blinks_, inputs.mqtt_published - published_);
        if ((policy.state & kMqttReceive) != 0U)
            blinks_ = add_activity(blinks_, inputs.mqtt_received - received_);
    }
    published_ = inputs.mqtt_published;
    received_ = inputs.mqtt_received;
    counters_seeded_ = true;

    if (blinks_ != 0U || inputs.hold_lit) {
        if (inputs.hold_lit) {
            blink_state_ = true;
        } else if (--blink_speed_ == 0U) {
            blink_speed_ = interval;
            blink_state_ = !blink_state_;
        }
        if ((policy.state & kManualOn) == 0U &&
            ((policy.state & kMqttActivity) != 0U || blinks_ > kProblemBlinks || blink_state_)) {
            set_link(blink_state_);
        }
        if (!blink_state_ && blinks_ != 0U) {
            --blinks_;
            if (blinks_ == kProblemBlinks)
                blinks_ = 0U;
        }
    }
    if ((policy.state & kPowerIndication) != 0U &&
        (io_.has_link_led() || (blinks_ == 0U && !inputs.hold_lit))) {
        set_power_leds((inputs.power & policy.mask) != 0U, inputs.power);
    }
}

std::uint8_t LedService::set_power(
    const std::uint8_t index, const std::uint8_t action, std::uint8_t state) {
    const auto led = io_.has_link_led() ? index : std::uint8_t{1U};
    const auto mask = static_cast<std::uint8_t>(1U << (led - 1U));
    state &= kManualOn; // Manual control disables power indication.
    switch (action) {
    case 0:
        led_power_ = static_cast<std::uint8_t>(led_power_ & ~mask);
        state = 0U;
        break;
    case 1:
        led_power_ = static_cast<std::uint8_t>(led_power_ | mask);
        state = kManualOn;
        break;
    default:
        led_power_ = static_cast<std::uint8_t>(led_power_ ^ mask);
        state = static_cast<std::uint8_t>(state ^ kManualOn);
        break;
    }
    blinks_ = 0U;
    if (!io_.has_link_led())
        set_power_leds((state & kManualOn) != 0U, 0U);
    else
        set_led(static_cast<std::uint8_t>(led - 1U), (led_power_ & mask) != 0U);
    return state;
}

bool LedService::power(const std::uint8_t index, const std::uint8_t state) const {
    if (!io_.has_link_led())
        return (state & kManualOn) != 0U;
    return (led_power_ & (1U << (index - 1U))) != 0U;
}

void LedService::state_changed(const std::uint8_t state) {
    if (state == 0U) {
        set_all(false);
        set_link(false);
    }
}

} // namespace newmota
