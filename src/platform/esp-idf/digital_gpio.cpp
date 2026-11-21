/*
  digital_gpio.cpp - ESP-IDF digital GPIO implementation for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/gpio.h"
#include "newmota/platform/gpio_detail.h"

#include "driver/gpio.h"

#include <cstdint>

namespace newmota::platform {
namespace {

/** @brief Maps a portable input pull to ESP-IDF GPIO flags. */
[[nodiscard]] GpioResult configure_pull(
    const GpioPull pull, gpio_config_t& configuration, std::uint16_t& requirements) {
    requirements = capability_mask(PinCapability::input);
    switch (pull) {
    case GpioPull::none:
        configuration.pull_up_en = GPIO_PULLUP_DISABLE;
        configuration.pull_down_en = GPIO_PULLDOWN_DISABLE;
        return GpioResult::success;
    case GpioPull::up:
        requirements |= capability_mask(PinCapability::pull_up);
        configuration.pull_up_en = GPIO_PULLUP_ENABLE;
        configuration.pull_down_en = GPIO_PULLDOWN_DISABLE;
        return GpioResult::success;
    case GpioPull::down:
        requirements |= capability_mask(PinCapability::pull_down);
        configuration.pull_up_en = GPIO_PULLUP_DISABLE;
        configuration.pull_down_en = GPIO_PULLDOWN_ENABLE;
        return GpioResult::success;
    }
    return GpioResult::unsupported;
}

} // namespace

/** @brief Configures an ESP pin as input with an explicit pull selection. */
GpioResult gpio_configure_input(const PinId pin, const GpioPull pull) {
    gpio_config_t configuration{};
    configuration.mode = GPIO_MODE_INPUT;
    configuration.intr_type = GPIO_INTR_DISABLE;
    std::uint16_t requirements = 0U;
    const auto pull_result = configure_pull(pull, configuration, requirements);
    if (pull_result != GpioResult::success) {
        return pull_result;
    }
    const auto validation = detail::validate_gpio(pin, requirements);
    if (validation != GpioResult::success) {
        return validation;
    }
    configuration.pin_bit_mask = std::uint64_t{1U} << pin.value();
    return gpio_config(&configuration) == ESP_OK ? GpioResult::success : GpioResult::failed;
}

/** @brief Configures an ESP output after preloading its output latch. */
GpioResult gpio_configure_output(const PinId pin, const bool initial_high) {
    const auto validation = detail::validate_gpio(pin, capability_mask(PinCapability::output));
    if (validation != GpioResult::success) {
        return validation;
    }
    const auto native_pin = static_cast<gpio_num_t>(pin.value());
    if (gpio_set_level(native_pin, initial_high ? 1U : 0U) != ESP_OK) {
        return GpioResult::failed;
    }
    gpio_config_t configuration{};
    configuration.pin_bit_mask = std::uint64_t{1U} << pin.value();
    configuration.mode = GPIO_MODE_OUTPUT;
    configuration.pull_up_en = GPIO_PULLUP_DISABLE;
    configuration.pull_down_en = GPIO_PULLDOWN_DISABLE;
    configuration.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&configuration) == ESP_OK ? GpioResult::success : GpioResult::failed;
}

/** @brief Reads an ESP pin without altering its current configuration. */
GpioResult gpio_read(const PinId pin, bool& high) {
    const auto validation = detail::validate_gpio(pin, capability_mask(PinCapability::input));
    if (validation != GpioResult::success) {
        return validation;
    }
    high = gpio_get_level(static_cast<gpio_num_t>(pin.value())) != 0;
    return GpioResult::success;
}

/** @brief Writes the ESP output latch of an output-capable pin. */
GpioResult gpio_write(const PinId pin, const bool high) {
    const auto validation = detail::validate_gpio(pin, capability_mask(PinCapability::output));
    if (validation != GpioResult::success) {
        return validation;
    }
    return gpio_set_level(static_cast<gpio_num_t>(pin.value()), high ? 1U : 0U) == ESP_OK
               ? GpioResult::success
               : GpioResult::failed;
}

} // namespace newmota::platform
