/*
  power_settings.h - persistent power settings shared by Newmota services

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file power_settings.h
 * @brief Persistent power settings shared by Newmota services.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace newmota {

inline constexpr std::size_t kMaximumPowerOutputs = 32;
inline constexpr std::size_t kMaximumInterlockGroups = kMaximumPowerOutputs / 2;

struct PowerSettings final {
    std::array<std::uint16_t, kMaximumPowerOutputs> pulse_times{};
    std::array<std::uint32_t, kMaximumInterlockGroups> interlock_groups{};
    std::uint32_t saved_state{};
    std::uint16_t save_data_seconds{1};
    std::uint16_t blink_time{10};
    std::uint16_t blink_count{10};
    std::uint8_t power_on_state{3};
    bool interlock_enabled{};
    /** Tasmota LedState: bit 0 power, bits 1–2 MQTT activity, bit 3 manual LedPower on. */
    std::uint8_t led_state{1};
    /** Tasmota LedMask: Power endpoints that light the power LED. */
    std::uint16_t led_mask{0xFFFFU};
    /** Tasmota LedPwmMode: LEDs driven by PWM levels instead of on/off. */
    std::uint8_t led_pwm_mask{};
    /** Tasmota LedPwmOn and LedPwmOff brightness, 0–255. */
    std::uint8_t led_pwm_on{255U};
    std::uint8_t led_pwm_off{};
};

inline constexpr PowerSettings kDefaultPowerSettings{};

} // namespace newmota
