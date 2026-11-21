/*
  led_service.h - Tasmota-compatible status and link LED behavior

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Source reference: Tasmota support_tasmota.ino (SetLedPowerIdx, SetLedPower, SetLedLink,
  Every250mSeconds) and support_command.ino (CmndLedPower, CmndLedState),
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
*/

/** @file led_service.h
 * @brief LedState, LedMask, LedPower, and network-status blinking for discrete LEDs.
 */

#pragma once

#include "newmota_features.h"
#if USE_PWM
#include "newmota/platform/pwm.h"

#include <array>
#endif
#include <cstdint>

namespace newmota {
class BasicIoDriver;

/** @brief Inputs sampled by the application every 250 ms. */
struct LedInputs final {
    /** Logical Power endpoint mask, relays and lights. */
    std::uint32_t power{};
    bool network_down{};
    /** MQTT is enabled but not connected. */
    bool mqtt_down{};
    /** A restart or update is pending; the link LED stays lit. */
    bool hold_lit{};
    /** Monotonic MQTT publish and receive counters for LedState activity blinks. */
    std::uint32_t mqtt_published{};
    std::uint32_t mqtt_received{};
};

/** @brief Persisted LED policy plus SetOption31. */
struct LedPolicy final {
    std::uint8_t state{1};
    std::uint16_t mask{0xFFFFU};
    /** SetOption31: do not blink the link LED for network or MQTT problems. */
    bool no_problem_blink{};
    /** LedPwmMode bits 0–3 and the LedPwmOn/LedPwmOff levels, 0–255. */
    std::uint8_t pwm_mask{};
    std::uint8_t pwm_on{255U};
    std::uint8_t pwm_off{};
    /** PWM carrier shared with Light outputs. */
    std::uint32_t carrier_hz{977U};
};

/** @brief Drives discrete LEDs and the link LED from power and connectivity state. */
class LedService final {
  public:
    constexpr explicit LedService(BasicIoDriver& io) : io_(io) {}

    /** @brief Applies the startup LED state after pins are acquired. */
    void start(const LedPolicy& policy);
    /** @brief Advances blinking and power indication; call every 250 ms. */
    void tick(const LedInputs& inputs, const LedPolicy& policy);

    /**
     * @brief Applies LedPower<index> off (0), on (1), or toggle (2).
     * @return The new LedState value to persist.
     */
    [[nodiscard]] std::uint8_t set_power(
        std::uint8_t index, std::uint8_t action, std::uint8_t state);
    /** @brief Reports the LedPower<index> state under the current LedState. */
    [[nodiscard]] bool power(std::uint8_t index, std::uint8_t state) const;
    /** @brief Applies a changed LedState; zero turns every LED off. */
    void state_changed(std::uint8_t state);
    /** @brief Reports whether LedPower may address LEDs beyond LED1. */
    [[nodiscard]] bool has_link_led() const;

    /** @brief Reports whether LedPwmMode<index> is set under the current policy. */
    [[nodiscard]] static bool pwm_mode(std::uint8_t index, std::uint8_t mask) {
        return (mask & (1U << (index - 1U))) != 0U;
    }

  private:
    void set_led(std::uint8_t index, bool on);
    void set_all(bool on);
    void set_power_leds(bool on, std::uint32_t power);
    void set_link(bool on);
    /** @brief UpdateLedPowerAll: rewrites every LED with its current state. */
    void refresh();
    /** @brief Claims or releases PWM outputs after LedPwmMode or level changes. */
    void sync_pwm(const LedPolicy& policy);

    BasicIoDriver& io_;
    std::uint8_t led_power_{};
    std::uint16_t blinks_{201};
    std::uint8_t blink_speed_{1};
    bool blink_state_{};
    bool counters_seeded_{};
    std::uint32_t published_{};
    std::uint32_t received_{};
    std::uint8_t pwm_mask_{};
    std::uint8_t pwm_on_{255U};
    std::uint8_t pwm_off_{};
#if USE_PWM
    std::array<platform::PwmHandle, 4> pwm_{};
#endif
};

} // namespace newmota
