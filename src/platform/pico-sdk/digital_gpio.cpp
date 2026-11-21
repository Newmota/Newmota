/*
  digital_gpio.cpp - Pico SDK digital GPIO implementation for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/gpio.h"
#include "newmota/platform/gpio_detail.h"

#include "hardware/gpio.h"

#include <cstdint>

namespace newmota::platform {
namespace {

/** @brief Adds the capability required by a portable pull selection. */
[[nodiscard]] GpioResult pull_requirements(const GpioPull pull, std::uint16_t& requirements) {
    requirements = capability_mask(PinCapability::input);
    switch (pull) {
    case GpioPull::none:
        return GpioResult::success;
    case GpioPull::up:
        requirements |= capability_mask(PinCapability::pull_up);
        return GpioResult::success;
    case GpioPull::down:
        requirements |= capability_mask(PinCapability::pull_down);
        return GpioResult::success;
    }
    return GpioResult::unsupported;
}

} // namespace

/** @brief Configures an RP2350 pin as input with an explicit pull. */
GpioResult gpio_configure_input(const PinId pin, const GpioPull pull) {
    std::uint16_t requirements = 0U;
    const auto pull_result = pull_requirements(pull, requirements);
    if (pull_result != GpioResult::success) {
        return pull_result;
    }
    const auto validation = detail::validate_gpio(pin, requirements);
    if (validation != GpioResult::success) {
        return validation;
    }
    const auto native_pin = static_cast<unsigned int>(pin.value());
    gpio_init(native_pin);
    switch (pull) {
    case GpioPull::none:
        gpio_disable_pulls(native_pin);
        break;
    case GpioPull::up:
        gpio_pull_up(native_pin);
        break;
    case GpioPull::down:
        gpio_pull_down(native_pin);
        break;
    }
    gpio_set_dir(native_pin, GPIO_IN);
    return GpioResult::success;
}

/** @brief Configures an RP2350 output after preloading its output latch. */
GpioResult gpio_configure_output(const PinId pin, const bool initial_high) {
    const auto validation = detail::validate_gpio(pin, capability_mask(PinCapability::output));
    if (validation != GpioResult::success) {
        return validation;
    }
    const auto native_pin = static_cast<unsigned int>(pin.value());
    gpio_init(native_pin);
    gpio_disable_pulls(native_pin);
    gpio_put(native_pin, initial_high);
    gpio_set_dir(native_pin, GPIO_OUT);
    return GpioResult::success;
}

/** @brief Reads an RP2350 pin without changing its configuration. */
GpioResult gpio_read(const PinId pin, bool& high) {
    const auto validation = detail::validate_gpio(pin, capability_mask(PinCapability::input));
    if (validation != GpioResult::success) {
        return validation;
    }
    high = gpio_get(static_cast<unsigned int>(pin.value()));
    return GpioResult::success;
}

/** @brief Writes the RP2350 output latch of an output-capable pin. */
GpioResult gpio_write(const PinId pin, const bool high) {
    const auto validation = detail::validate_gpio(pin, capability_mask(PinCapability::output));
    if (validation != GpioResult::success) {
        return validation;
    }
    gpio_put(static_cast<unsigned int>(pin.value()), high);
    return GpioResult::success;
}

} // namespace newmota::platform
