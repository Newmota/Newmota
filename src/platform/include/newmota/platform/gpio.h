/*
  gpio.h - portable digital GPIO boundary for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "newmota/core/hardware/pin.h"

#include <cstdint>

namespace newmota::platform {

enum class GpioPull : std::uint8_t {
    none,
    up,
    down,
};

enum class GpioResult : std::uint8_t {
    success,
    unavailable,
    unsupported,
    failed,
};

/** @brief Reports whether the resolved board topology exposes @p pin. */
[[nodiscard]] bool gpio_available(PinId pin);
/** @brief Returns the resolved digital capability mask for @p pin. */
[[nodiscard]] std::uint16_t gpio_capabilities(PinId pin);
/** @brief Reports whether the console uses @p pin until a configuration claims it. */
[[nodiscard]] bool gpio_console_pin(PinId pin);
/** @brief Configures an available pin as a digital input with @p pull. */
[[nodiscard]] GpioResult gpio_configure_input(PinId pin, GpioPull pull);
/** @brief Configures an available pin as output with a defined initial level. */
[[nodiscard]] GpioResult gpio_configure_output(PinId pin, bool initial_high);
/** @brief Reads the digital level without changing the pin configuration. */
[[nodiscard]] GpioResult gpio_read(PinId pin, bool& high);
/** @brief Writes the digital output latch for an output-capable pin. */
[[nodiscard]] GpioResult gpio_write(PinId pin, bool high);

} // namespace newmota::platform
