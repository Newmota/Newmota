/*
  gpio_topology.cpp - resolved topology access for Newmota GPIO adapters

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/gpio.h"
#include "newmota/platform/gpio_detail.h"

#include "pin_topology.h"

#include <cstdint>

namespace newmota::platform {
namespace {

static_assert(capability_mask(PinCapability::input) == generated::hardware::kInput);
static_assert(capability_mask(PinCapability::output) == generated::hardware::kOutput);
static_assert(capability_mask(PinCapability::pull_up) == generated::hardware::kPullUp);
static_assert(capability_mask(PinCapability::pull_down) == generated::hardware::kPullDown);
static_assert(capability_mask(PinCapability::open_drain) == generated::hardware::kOpenDrain);
static_assert(capability_mask(PinCapability::interrupt) == generated::hardware::kInterrupt);
static_assert(capability_mask(PinCapability::analog) == generated::hardware::kAnalog);

/** @brief Finds the generated topology record for one physical pin. */
[[nodiscard]] const generated::hardware::PinTopology* topology_for(const PinId pin) {
    for (const auto& candidate : generated::hardware::kPins) {
        if (candidate.id == pin.value()) {
            return &candidate;
        }
    }
    return nullptr;
}

bool console_pins_released{};

} // namespace

/** @brief Reports whether the resolved board topology exposes @p pin, console pins included. */
bool gpio_available(const PinId pin) {
    const auto* const topology = topology_for(pin);
    return topology != nullptr && (topology->flags & generated::hardware::kPinAvailable) != 0U;
}

/** @brief Reports whether the console uses @p pin until a template assigns it. */
bool gpio_console_pin(const PinId pin) {
    const auto* const topology = topology_for(pin);
    constexpr auto kFlags = generated::hardware::kPinAvailable | generated::hardware::kPinConsole;
    return topology != nullptr && (topology->flags & kFlags) == kFlags;
}

/** @brief Returns the available digital capability mask for @p pin. */
std::uint16_t gpio_capabilities(const PinId pin) {
    const auto* const topology = topology_for(pin);
    return topology != nullptr && (topology->flags & generated::hardware::kPinAvailable) != 0U
               ? topology->capabilities
               : 0U;
}

namespace detail {

/** @brief Validates availability and all requested topology capabilities. */
GpioResult validate_gpio(const PinId pin, const std::uint16_t requirements) {
    const auto* const topology = topology_for(pin);
    if (topology == nullptr || (topology->flags & generated::hardware::kPinAvailable) == 0U) {
        return GpioResult::unavailable;
    }
    if ((topology->flags & generated::hardware::kPinConsole) != 0U && !console_pins_released) {
        return GpioResult::unavailable;
    }
    return (topology->capabilities & requirements) == requirements ? GpioResult::success
                                                                   : GpioResult::unsupported;
}

/** @brief Makes console pins available after the platform stopped its console. */
void release_console_pins() {
    console_pins_released = true;
}

} // namespace detail
} // namespace newmota::platform
