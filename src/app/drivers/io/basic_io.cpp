/*
  basic_io.cpp - basic digital device driver for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  ButtonProbe and SwitchProbe reference: Tasmota support_button_v4.ino and
  support_switch_v4.ino, Copyright (C) 2022 Federico Leoni and Theo Arends,
  GPL version 3 or later.
*/

/** @file basic_io.cpp
 * @brief Owns core relay/button/switch GPIO; optional timing gates preserve its output policies.
 */

#include "drivers/io/basic_io.h"
#include "newmota_features.h"
#if USE_ZERO_CROSS
#include "drivers/gpio/zero_cross.h"
#endif

#include <cstddef>
#include <cstdint>

namespace newmota {
namespace {

/** @brief Tests one compact modifier on a resolved pin binding. */
[[nodiscard]] constexpr bool has_modifier(const PinBinding& binding, const PinModifier modifier) {
    return (binding.modifiers & static_cast<std::uint8_t>(modifier)) != 0U;
}

/** @brief Sets or clears one bit in a compact 32-channel state mask. */
constexpr void set_mask_bit(std::uint32_t& mask, const std::size_t index, const bool value) {
    const std::uint32_t bit = 1U << index;
    mask = value ? mask | bit : mask & ~bit;
}

/** @brief Reads one bit from a compact 32-channel state mask. */
[[nodiscard]] constexpr bool mask_bit(const std::uint32_t mask, const std::size_t index) {
    return (mask & (1U << index)) != 0U;
}

#ifndef NEWMOTA_IMAGE_SAFEBOOT
/** @brief Converts a resolved button or switch modifier set into a native pull mode. */
[[nodiscard]] constexpr platform::GpioPull input_pull(const PinBinding& binding) {
    if (has_modifier(binding, PinModifier::pull_down)) {
        return platform::GpioPull::down;
    }
    return has_modifier(binding, PinModifier::no_pull) ? platform::GpioPull::none
                                                       : platform::GpioPull::up;
}
#endif

} // namespace

#ifndef NEWMOTA_IMAGE_SAFEBOOT
static_assert(BasicIoDriver::kMaximumSwitches == BasicIoDriver::kMaximumButtons);

/** @brief Configures one input pin and records its initial logical level. */
platform::GpioResult BasicIoDriver::add_input(
    InputBank& bank, const PinBinding& binding, const std::size_t channel) {
    auto status = platform::gpio_configure_input(binding.pin, input_pull(binding));
    bool high = false;
    if (status == platform::GpioResult::success) {
        status = platform::gpio_read(binding.pin, high);
    }
    if (status == platform::GpioResult::success) {
        const bool inverted = has_modifier(binding, PinModifier::inverted);
        bank.pins[channel] = binding.pin;
        set_mask_bit(bank.inverted, channel, inverted);
        set_mask_bit(bank.pressed, channel, high == inverted);
        ++bank.count;
    }
    return status;
}
#endif

/** @brief Acquires resolved relay, button, switch, and LED pins in safe states. */
BasicIoInitializationResult BasicIoDriver::initialize(const Hardware& hardware) {
#if USE_ZERO_CROSS
    zero_cross::initialize(hardware);
#endif
    relay_pins_.fill(PinId{});
    led_pins_.fill(PinId{});
    link_led_pin_ = PinId{};
    link_led_inverted_ = false;
    relay_inverted_ = 0U;
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    buttons_ = {};
#endif
#if USE_SWITCHES
    switches_ = {};
#endif
    led_inverted_ = 0U;
    relay_count_ = 0U;
    led_count_ = 0U;
    BasicIoInitializationResult result{};

    for (std::size_t index = 0; index < hardware.active_count(); ++index) {
        const auto* const binding = hardware.function_binding(index);
        if (binding == nullptr || binding->instance == 0U) {
            continue;
        }
        const std::size_t channel = binding->instance - 1U;
        platform::GpioResult status = platform::GpioResult::unsupported;
        switch (binding->role) {
        case PinRole::relay:
            if (channel >= relay_pins_.size()) {
                break;
            }
            status = platform::gpio_configure_output(
                binding->pin, has_modifier(*binding, PinModifier::inverted));
            if (status == platform::GpioResult::success) {
                relay_pins_[channel] = binding->pin;
                set_mask_bit(
                    relay_inverted_, channel, has_modifier(*binding, PinModifier::inverted));
                ++relay_count_;
                ++result.relay_count;
            }
            break;
#ifndef NEWMOTA_IMAGE_SAFEBOOT
        case PinRole::button:
            if (channel >= buttons_.pins.size()) {
                break;
            }
            status = add_input(buttons_, *binding, channel);
            if (status == platform::GpioResult::success) {
                ++result.button_count;
            }
            break;
#else
        case PinRole::button:
            continue; // Recovery never configures or reads application buttons.
#endif
#if USE_SWITCHES
        case PinRole::switch_input:
            if (channel >= switches_.pins.size()) {
                break;
            }
            status = add_input(switches_, *binding, channel);
            if (status == platform::GpioResult::success) {
                ++result.switch_count;
            }
            break;
#else
        case PinRole::switch_input:
            continue; // The pin stays unconfigured without USE_SWITCHES.
#endif
        case PinRole::status_led:
            if (channel >= led_pins_.size()) {
                break;
            }
            status = platform::gpio_configure_output(
                binding->pin, has_modifier(*binding, PinModifier::inverted));
            if (status == platform::GpioResult::success) {
                led_pins_[channel] = binding->pin;
                if (has_modifier(*binding, PinModifier::inverted)) {
                    led_inverted_ |= static_cast<std::uint8_t>(1U << channel);
                }
                ++led_count_;
                ++result.led_count;
            }
            break;
        case PinRole::link_led:
            status = platform::gpio_configure_output(
                binding->pin, has_modifier(*binding, PinModifier::inverted));
            if (status == platform::GpioResult::success) {
                link_led_pin_ = binding->pin;
                link_led_inverted_ = has_modifier(*binding, PinModifier::inverted);
            }
            break;
        case PinRole::i2c_scl:
        case PinRole::buzzer:
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
        case PinRole::led_strip:
        case PinRole::i2c_sda:
        case PinRole::dht11:
        case PinRole::dht22:
        case PinRole::si7021:
        case PinRole::i2s_dout:
        case PinRole::i2s_bclk:
        case PinRole::i2s_ws:
        case PinRole::i2s_din:
        case PinRole::i2s_mclk:
        case PinRole::pwm:
        case PinRole::none:
        case PinRole::input:
        case PinRole::fixed_output:
            continue;
        }
        if (status != platform::GpioResult::success) {
            ++result.failure_count;
        }
    }
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    buttons_.filtered_pressed = buttons_.pressed;
#endif
#if USE_SWITCHES
    switches_.filtered_pressed = switches_.pressed;
#endif
    return result;
}

/** @brief Reports whether a one-based relay instance is configured. */
bool BasicIoDriver::has_relay(const std::uint8_t instance) const {
    if (instance == 0U || instance > relay_pins_.size())
        return false;
#if USE_TUYA_MCU
    if (mask_bit(external_relays_, instance - 1U))
        return true;
#endif
    return relay_pins_[instance - 1U].valid();
}

#if USE_TUYA_MCU
std::uint8_t BasicIoDriver::relay_count() const {
    std::uint8_t count = 0U;
    for (std::size_t instance = 1U; instance <= relay_pins_.size(); ++instance)
        if (has_relay(static_cast<std::uint8_t>(instance)))
            ++count;
    return count;
}
#endif

#ifndef NEWMOTA_IMAGE_SAFEBOOT
/** @brief Reports whether a one-based button instance is configured. */
bool BasicIoDriver::has_button(const std::uint8_t instance) const {
    return instance != 0U && instance <= buttons_.pins.size() &&
           buttons_.pins[instance - 1U].valid();
}
#endif
#if USE_SWITCHES
/** @brief Reports whether a one-based switch instance is configured. */
bool BasicIoDriver::has_switch(const std::uint8_t instance) const {
    if (instance == 0U || instance > switches_.pins.size())
        return false;
#if USE_TUYA_MCU
    if (mask_bit(external_switches_ & external_seen_, instance - 1U))
        return true;
#endif
    return switches_.pins[instance - 1U].valid();
}

#if USE_TUYA_MCU
std::uint8_t BasicIoDriver::switch_count() const {
    std::uint8_t count = switches_.count;
    for (std::size_t index = 0U; index < switches_.pins.size(); ++index)
        if (mask_bit(external_switches_, index) && !switches_.pins[index].valid())
            ++count;
    return count;
}

void BasicIoDriver::set_external_switch(const std::uint8_t instance, const bool high) {
    if (instance == 0U || instance > switches_.pins.size() ||
        !mask_bit(external_switches_, instance - 1U))
        return;
    set_mask_bit(external_seen_, instance - 1U, true);
    set_mask_bit(external_levels_, instance - 1U, high);
}
#endif

std::uint32_t BasicIoDriver::configured_switches() const {
    std::uint32_t mask = 0U;
    for (std::size_t index = 0U; index < switches_.pins.size(); ++index)
        set_mask_bit(mask, index, switches_.pins[index].valid());
#if USE_TUYA_MCU
    mask |= external_switches_;
#endif
    return mask;
}

/** @brief Filters switch samples; Tasmota's SwitchProbe matches ButtonProbe. */
void BasicIoDriver::probe_switches(const std::uint32_t now, const std::uint16_t debounce) {
    // Switches have no edge delivery, so a debounce change restarts from the filtered level.
    switches_.pressed = switches_.filtered_pressed;
    probe(switches_, now, debounce);
}
#endif

/** @brief Writes one logical relay state through its inversion mapping. */
platform::GpioResult BasicIoDriver::write_relay(const std::uint8_t instance, const bool on) {
    if (!has_relay(instance)) {
        return platform::GpioResult::unavailable;
    }
    const std::size_t index = instance - 1U;
#if USE_TUYA_MCU
    if (mask_bit(external_relays_, index))
        return external_writer_ != nullptr && external_writer_(external_context_, instance, on)
                   ? platform::GpioResult::success
                   : platform::GpioResult::unavailable;
#endif
#if USE_ZERO_CROSS
    const auto timing = zero_cross::begin(relay_pins_[index]);
#endif
    const auto result =
        platform::gpio_write(relay_pins_[index], on != mask_bit(relay_inverted_, index));
#if USE_ZERO_CROSS
    zero_cross::end(timing);
#endif
    return result;
}

/** @brief Writes one logical LED state through its inversion mapping. */
platform::GpioResult BasicIoDriver::write_led(const std::uint8_t instance, const bool on) {
    if (instance == 0U || instance > led_pins_.size() || !led_pins_[instance - 1U].valid()) {
        return platform::GpioResult::unavailable;
    }
    const std::size_t index = instance - 1U;
    const bool inverted = (led_inverted_ & static_cast<std::uint8_t>(1U << index)) != 0U;
    return platform::gpio_write(led_pins_[index], on != inverted);
}

/** @brief Reports whether a one-based LED instance is configured. */
bool BasicIoDriver::has_led(const std::uint8_t instance) const {
    return instance != 0U && instance <= led_pins_.size() && led_pins_[instance - 1U].valid();
}

/** @brief Writes the link LED through its inversion mapping. */
platform::GpioResult BasicIoDriver::write_link_led(const bool on) {
    if (!link_led_pin_.valid()) {
        return platform::GpioResult::unavailable;
    }
    return platform::gpio_write(link_led_pin_, on != link_led_inverted_);
}

#ifndef NEWMOTA_IMAGE_SAFEBOOT
/** @brief Filters real GPIO samples; elapsed time never fabricates unobserved samples. */
void BasicIoDriver::probe_buttons(const std::uint32_t now, const std::uint16_t debounce) {
    probe(buttons_, now, debounce);
}

/** @brief Runs the logical HIGH/LOW integrator over one bank once per 10 ms. */
void BasicIoDriver::probe(InputBank& bank, const std::uint32_t now, const std::uint16_t debounce) {
    if (bank.count == 0U) {
        return;
    }
    constexpr std::uint32_t kProbeInterval = 10U;
    const auto threshold = static_cast<std::uint8_t>(debounce / kProbeInterval);
    if (bank.debounce != debounce) {
        bank.debounce = debounce;
        bank.filtered_pressed = bank.pressed;
        for (std::size_t index = 0U; index < bank.filters.size(); ++index) {
            bank.filters[index] = mask_bit(bank.pressed, index) ? 0U : threshold;
        }
        bank.probe_at = now + kProbeInterval;
        return;
    }
    if (static_cast<std::int32_t>(now - bank.probe_at) < 0) {
        return;
    }
    bank.probe_at = now + kProbeInterval;
    const auto flags = debounce % 10U;
    for (std::size_t index = 0; index < bank.pins.size(); ++index) {
        if (!bank.pins[index].valid()) {
            continue;
        }
        bool high = false;
        if (platform::gpio_read(bank.pins[index], high) != platform::GpioResult::success) {
            continue;
        }
        const bool pressed = high == mask_bit(bank.inverted, index);
        auto& count = bank.filters[index];
        const bool filtered_pressed = mask_bit(bank.filtered_pressed, index);
        if (!pressed) { // Logical HIGH, after normal/inverted polarity is applied.
            if ((flags & 1U) != 0U && !filtered_pressed) {
                count = threshold;
            }
            if (count < threshold && ++count == threshold) {
                set_mask_bit(bank.filtered_pressed, index, false);
            }
        } else {
            if ((flags & 2U) != 0U && filtered_pressed) {
                count = 0U;
            }
            if (count > 0U && --count == 0U) {
                set_mask_bit(bank.filtered_pressed, index, true);
            }
        }
    }
}

/** @brief Emits filtered edges without allocating storage or rereading GPIO. */
void BasicIoDriver::poll_buttons(const ButtonEdgeHandler handler, void* const context) {
    if (handler == nullptr) {
        return;
    }
    for (std::size_t index = 0; index < buttons_.pins.size(); ++index) {
        if (!buttons_.pins[index].valid()) {
            continue;
        }
        const bool pressed = mask_bit(buttons_.filtered_pressed, index);
        if (pressed == mask_bit(buttons_.pressed, index)) {
            continue;
        }
        set_mask_bit(buttons_.pressed, index, pressed);
        handler(context, {static_cast<std::uint8_t>(index + 1U), pressed});
    }
}

/** @brief Returns the last delivered filtered logical state of one button. */
bool BasicIoDriver::button_pressed(const std::uint8_t instance) const {
    return has_button(instance) && mask_bit(buttons_.pressed, instance - 1U);
}
#endif

} // namespace newmota
