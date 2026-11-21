/*
  hardware_runtime.cpp - startup GPIO application for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "core/hardware/hardware.h"

#include "newmota/platform/gpio.h"
#include "newmota/platform/platform.h"

#include <cstddef>
#include <cstdint>

namespace newmota {
namespace {

static_assert(kMaximumHardwarePins <= 64U);

/** @brief Tests one compact modifier bit on a resolved binding. */
[[nodiscard]] constexpr bool has_modifier(const PinBinding& binding, const PinModifier modifier) {
    return (binding.modifiers & static_cast<std::uint8_t>(modifier)) != 0U;
}

/** @brief Maps a native GPIO result into the portable startup diagnostic. */
[[nodiscard]] PinInitializationError initialization_error(const platform::GpioResult result) {
    switch (result) {
    case platform::GpioResult::unavailable:
        return PinInitializationError::unavailable;
    case platform::GpioResult::unsupported:
        return PinInitializationError::unsupported;
    case platform::GpioResult::failed:
        return PinInitializationError::failed;
    case platform::GpioResult::success:
        break;
    }
    return PinInitializationError::failed;
}

/** @brief Packs a function-order index and two-bit error into one byte. */
[[nodiscard]] constexpr std::uint8_t pack_failure(
    const std::size_t binding_index, const PinInitializationError error) {
    return static_cast<std::uint8_t>(binding_index) |
           static_cast<std::uint8_t>(static_cast<std::uint8_t>(error) << 6U);
}

/** @brief Initializes one core-owned binding without claiming driver roles. */
[[nodiscard]] platform::GpioResult initialize_core_binding(
    const PinBinding& binding, bool& owned, bool& invalid_role) {
    owned = true;
    invalid_role = false;
    switch (binding.role) {
    case PinRole::input: {
        const auto pull = has_modifier(binding, PinModifier::pull_down) ? platform::GpioPull::down
                                                                        : platform::GpioPull::none;
        return platform::gpio_configure_input(binding.pin, pull);
    }
    case PinRole::fixed_output: {
        const bool initial_high = has_modifier(binding, PinModifier::initial_high);
        const bool initial_low = has_modifier(binding, PinModifier::initial_low);
        if (initial_high == initial_low) {
            invalid_role = true;
            return platform::GpioResult::unsupported;
        }
        return platform::gpio_configure_output(binding.pin, initial_high);
    }
    case PinRole::i2c_scl:
    case PinRole::led_strip:
    case PinRole::i2c_sda:
    case PinRole::pwm:
    case PinRole::buzzer:
    case PinRole::none:
    case PinRole::button:
    case PinRole::switch_input:
    case PinRole::relay:
    case PinRole::status_led:
    case PinRole::link_led:
    case PinRole::gps_rx:
    case PinRole::gps_tx:
    case PinRole::energy_rx:
    case PinRole::energy_tx:
    case PinRole::energy_cf:
    case PinRole::energy_cf1:
    case PinRole::energy_sel:
    case PinRole::energy_irq:
    case PinRole::energy_reset:
    case PinRole::energy_cs:
    case PinRole::energy_voltage:
    case PinRole::energy_current:
    case PinRole::energy_option:
    case PinRole::dht11:
    case PinRole::dht22:
    case PinRole::si7021:
    case PinRole::i2s_dout:
    case PinRole::i2s_bclk:
    case PinRole::i2s_ws:
    case PinRole::i2s_din:
    case PinRole::i2s_mclk:
        owned = false;
        break;
    }
    return platform::GpioResult::unsupported;
}

} // namespace

/** @brief Resolves startup hardware and initializes only core-owned GPIO roles. */
HardwareStartupResult HardwareRuntime::initialize(const HardwareConfiguration& configuration) {
    failures_.fill(0U);
    failure_count_ = 0U;

    HardwareStartupResult result{};
    result.resolution =
        HardwareResolver::resolve(configuration, ResolutionPolicy::tolerant, hardware_);
    if (!result.resolved()) {
        return result;
    }

    // The template wins over the console: stop it before any pin is driven.
    for (std::size_t index = 0; index < hardware_.active_count(); ++index) {
        const auto* const binding = hardware_.function_binding(index);
        if (binding != nullptr && platform::gpio_console_pin(binding->pin)) {
            if (platform::release_console())
                result.console_pin = binding->pin;
            break;
        }
    }

    for (std::size_t index = 0; index < hardware_.active_count(); ++index) {
        const auto* const binding = hardware_.function_binding(index);
        if (binding == nullptr) {
            continue;
        }
        bool owned = false;
        bool invalid_role = false;
        const auto status = initialize_core_binding(*binding, owned, invalid_role);
        if (!owned) {
            continue;
        }
        if (status == platform::GpioResult::success) {
            ++result.initialized_count;
            continue;
        }
        failures_[failure_count_++] = pack_failure(index,
            invalid_role ? PinInitializationError::invalid_role : initialization_error(status));
    }
    result.failure_count = failure_count_;
    return result;
}

/** @brief Decodes one retained native GPIO startup failure by index. */
bool HardwareRuntime::failure(const std::size_t index, PinInitializationFailure& output) const {
    if (index >= failure_count_) {
        return false;
    }
    const std::uint8_t packed = failures_[index];
    const auto* const binding = hardware_.function_binding(packed & 0x3fU);
    if (binding == nullptr) {
        return false;
    }
    output = {
        binding->pin,
        binding->function,
        static_cast<PinInitializationError>(packed >> 6U),
    };
    return true;
}

} // namespace newmota
