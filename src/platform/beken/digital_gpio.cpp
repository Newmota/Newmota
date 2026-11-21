/*
  digital_gpio.cpp - Beken digital GPIO implementation for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/gpio.h"
#include "newmota/platform/gpio_detail.h"

extern "C" {
#include "gpio_pub.h"
}

#include <cstdint>

namespace newmota::platform {
namespace {

/** @brief Maps a portable input pull to the Beken GPIO mode. */
[[nodiscard]] GpioResult input_mode(
    const GpioPull pull, std::uint32_t& mode, std::uint16_t& requirements) {
    requirements = capability_mask(PinCapability::input);
    switch (pull) {
    case GpioPull::none:
        mode = GMODE_INPUT;
        return GpioResult::success;
    case GpioPull::up:
        mode = GMODE_INPUT_PULLUP;
        requirements |= capability_mask(PinCapability::pull_up);
        return GpioResult::success;
    case GpioPull::down:
        mode = GMODE_INPUT_PULLDOWN;
        requirements |= capability_mask(PinCapability::pull_down);
        return GpioResult::success;
    }
    return GpioResult::unsupported;
}

} // namespace

/** @brief Configures a Beken pin as input with an explicit pull. */
GpioResult gpio_configure_input(const PinId pin, const GpioPull pull) {
    std::uint32_t mode = 0U;
    std::uint16_t requirements = 0U;
    const auto mode_result = input_mode(pull, mode, requirements);
    if (mode_result != GpioResult::success) {
        return mode_result;
    }
    const auto validation = detail::validate_gpio(pin, requirements);
    if (validation != GpioResult::success) {
        return validation;
    }
    gpio_config(pin.value(), mode);
    return GpioResult::success;
}

/** @brief Configures a Beken output after preloading its output latch. */
GpioResult gpio_configure_output(const PinId pin, const bool initial_high) {
    const auto validation = detail::validate_gpio(pin, capability_mask(PinCapability::output));
    if (validation != GpioResult::success) {
        return validation;
    }
    gpio_output(pin.value(), initial_high ? 1U : 0U);
    gpio_config(pin.value(), GMODE_OUTPUT);
    return GpioResult::success;
}

/** @brief Reads a Beken pin without changing its configuration. */
GpioResult gpio_read(const PinId pin, bool& high) {
    const auto validation = detail::validate_gpio(pin, capability_mask(PinCapability::input));
    if (validation != GpioResult::success) {
        return validation;
    }
    high = gpio_input(pin.value()) != 0U;
    return GpioResult::success;
}

/** @brief Writes the Beken output latch of an output-capable pin. */
GpioResult gpio_write(const PinId pin, const bool high) {
    const auto validation = detail::validate_gpio(pin, capability_mask(PinCapability::output));
    if (validation != GpioResult::success) {
        return validation;
    }
    gpio_output(pin.value(), high ? 1U : 0U);
    return GpioResult::success;
}

} // namespace newmota::platform
