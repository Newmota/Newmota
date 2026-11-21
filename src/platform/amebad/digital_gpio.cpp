/*
  digital_gpio.cpp - Realtek AmebaD digital GPIO implementation for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/gpio.h"
#include "newmota/platform/gpio_detail.h"

extern "C" {
#include "ameba_soc.h"
}

#include <cstdint>

namespace newmota::platform {
namespace {

/** @brief Maps a portable input pull to the AmebaD SDK value. */
[[nodiscard]] GpioResult pull_configuration(
    const GpioPull pull, std::uint32_t& native_pull, std::uint16_t& requirements) {
    requirements = capability_mask(PinCapability::input);
    switch (pull) {
    case GpioPull::none:
        native_pull = GPIO_PuPd_NOPULL;
        return GpioResult::success;
    case GpioPull::up:
        native_pull = GPIO_PuPd_UP;
        requirements |= capability_mask(PinCapability::pull_up);
        return GpioResult::success;
    case GpioPull::down:
        native_pull = GPIO_PuPd_DOWN;
        requirements |= capability_mask(PinCapability::pull_down);
        return GpioResult::success;
    }
    return GpioResult::unsupported;
}

} // namespace

/** @brief Configures an AmebaD pin as input with an explicit pull. */
GpioResult gpio_configure_input(const PinId pin, const GpioPull pull) {
    std::uint32_t native_pull = 0U;
    std::uint16_t requirements = 0U;
    const auto pull_result = pull_configuration(pull, native_pull, requirements);
    if (pull_result != GpioResult::success) {
        return pull_result;
    }
    const auto validation = detail::validate_gpio(pin, requirements);
    if (validation != GpioResult::success) {
        return validation;
    }
    const auto native_pin = static_cast<std::uint32_t>(pin.value());
    Pinmux_Config(static_cast<std::uint8_t>(native_pin), PINMUX_FUNCTION_GPIO);
    GPIO_InitTypeDef configuration{};
    configuration.GPIO_Pin = native_pin;
    configuration.GPIO_Mode = GPIO_Mode_IN;
    configuration.GPIO_PuPd = native_pull;
    GPIO_Init(&configuration);
    return GpioResult::success;
}

/** @brief Configures an AmebaD output after preloading its output latch. */
GpioResult gpio_configure_output(const PinId pin, const bool initial_high) {
    const auto validation = detail::validate_gpio(pin, capability_mask(PinCapability::output));
    if (validation != GpioResult::success) {
        return validation;
    }
    const auto native_pin = static_cast<std::uint32_t>(pin.value());
    Pinmux_Config(static_cast<std::uint8_t>(native_pin), PINMUX_FUNCTION_GPIO);
    GPIO_WriteBit(native_pin, initial_high ? 1U : 0U);
    GPIO_InitTypeDef configuration{};
    configuration.GPIO_Pin = native_pin;
    configuration.GPIO_Mode = GPIO_Mode_OUT;
    configuration.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(&configuration);
    return GpioResult::success;
}

/** @brief Reads an AmebaD pin without changing its configuration. */
GpioResult gpio_read(const PinId pin, bool& high) {
    const auto validation = detail::validate_gpio(pin, capability_mask(PinCapability::input));
    if (validation != GpioResult::success) {
        return validation;
    }
    high = GPIO_ReadDataBit(pin.value()) != 0U;
    return GpioResult::success;
}

/** @brief Writes the AmebaD output latch of an output-capable pin. */
GpioResult gpio_write(const PinId pin, const bool high) {
    const auto validation = detail::validate_gpio(pin, capability_mask(PinCapability::output));
    if (validation != GpioResult::success) {
        return validation;
    }
    GPIO_WriteBit(pin.value(), high ? 1U : 0U);
    return GpioResult::success;
}

} // namespace newmota::platform
