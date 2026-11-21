/*
  digital_gpio.cpp - BouffaloSDK digital GPIO implementation for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/gpio.h"
#include "newmota/platform/gpio_detail.h"

extern "C" {
#include "bflb_gpio.h"
}

#include <cstdint>

namespace newmota::platform {
namespace {

// entry.cpp runs constructors after board_init() registers the SDK devices.
bflb_device_s* const g_gpio_device = bflb_device_get_by_name("gpio");

/** @brief Returns the SDK-owned GPIO device, if board startup registered it. */
[[nodiscard]] bflb_device_s* gpio_device() {
    return g_gpio_device;
}

/** @brief Maps a portable input pull to Bouffalo LHAL configuration bits. */
[[nodiscard]] GpioResult pull_configuration(
    const GpioPull pull, std::uint32_t& configuration, std::uint16_t& requirements) {
    requirements = capability_mask(PinCapability::input);
    switch (pull) {
    case GpioPull::none:
        configuration = GPIO_FLOAT;
        return GpioResult::success;
    case GpioPull::up:
        configuration = GPIO_PULLUP;
        requirements |= capability_mask(PinCapability::pull_up);
        return GpioResult::success;
    case GpioPull::down:
        configuration = GPIO_PULLDOWN;
        requirements |= capability_mask(PinCapability::pull_down);
        return GpioResult::success;
    }
    return GpioResult::unsupported;
}

} // namespace

/** @brief Configures a Bouffalo pin as input with an explicit pull. */
GpioResult gpio_configure_input(const PinId pin, const GpioPull pull) {
    std::uint32_t pull_bits = 0U;
    std::uint16_t requirements = 0U;
    const auto pull_result = pull_configuration(pull, pull_bits, requirements);
    if (pull_result != GpioResult::success) {
        return pull_result;
    }
    const auto validation = detail::validate_gpio(pin, requirements);
    if (validation != GpioResult::success) {
        return validation;
    }
    auto* const device = gpio_device();
    if (device == nullptr) {
        return GpioResult::failed;
    }
    bflb_gpio_init(device, static_cast<std::uint8_t>(pin.value()),
        GPIO_INPUT | pull_bits | GPIO_SMT_EN | GPIO_DRV_0);
    return GpioResult::success;
}

/** @brief Configures a Bouffalo output after preloading its output latch. */
GpioResult gpio_configure_output(const PinId pin, const bool initial_high) {
    const auto validation = detail::validate_gpio(pin, capability_mask(PinCapability::output));
    if (validation != GpioResult::success) {
        return validation;
    }
    auto* const device = gpio_device();
    if (device == nullptr) {
        return GpioResult::failed;
    }
    const auto native_pin = static_cast<std::uint8_t>(pin.value());
    if (initial_high) {
        bflb_gpio_set(device, native_pin);
    } else {
        bflb_gpio_reset(device, native_pin);
    }
    bflb_gpio_init(device, native_pin, GPIO_OUTPUT | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_0);
    return GpioResult::success;
}

/** @brief Reads a Bouffalo pin without changing its configuration. */
GpioResult gpio_read(const PinId pin, bool& high) {
    const auto validation = detail::validate_gpio(pin, capability_mask(PinCapability::input));
    if (validation != GpioResult::success) {
        return validation;
    }
    auto* const device = gpio_device();
    if (device == nullptr) {
        return GpioResult::failed;
    }
    high = bflb_gpio_read(device, static_cast<std::uint8_t>(pin.value()));
    return GpioResult::success;
}

/** @brief Writes the Bouffalo output latch of an output-capable pin. */
GpioResult gpio_write(const PinId pin, const bool high) {
    const auto validation = detail::validate_gpio(pin, capability_mask(PinCapability::output));
    if (validation != GpioResult::success) {
        return validation;
    }
    auto* const device = gpio_device();
    if (device == nullptr) {
        return GpioResult::failed;
    }
    if (high) {
        bflb_gpio_set(device, static_cast<std::uint8_t>(pin.value()));
    } else {
        bflb_gpio_reset(device, static_cast<std::uint8_t>(pin.value()));
    }
    return GpioResult::success;
}

} // namespace newmota::platform
