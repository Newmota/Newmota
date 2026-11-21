/*
  edge_capture.cpp - Bouffalo native GPIO edge capture and microsecond snapshots

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file edge_capture.cpp
 * @brief Bouffalo native GPIO edge capture and microsecond snapshots.
 */

#include "newmota/platform/edge_capture_state.h"
extern "C" {
#include "bflb_gpio.h"
#include "bflb_irq.h"
#include "bflb_mtimer.h"
}

namespace newmota::platform::edge_capture {
namespace {
detail::Slots slots;

class Guard {
  public:
    Guard() : saved_(bflb_irq_save()) {}

    ~Guard() { bflb_irq_restore(saved_); }

  private:
    std::uintptr_t saved_;
};

void interrupt(std::uint8_t pin) {
    Guard guard;
    const auto now = bflb_mtimer_get_time_us();
    for (auto& state : slots.states)
        if (state.active && state.configuration.pin.value() == pin)
            detail::record(state, now);
}
} // namespace

std::uint64_t monotonic_microseconds() {
    return bflb_mtimer_get_time_us();
}

Result open(const Configuration& config, Handle& handle) {
    handle = {};
#if defined(BL602) || defined(BL702)
    if (config.edge == Edge::both)
        return Result::unsupported;
#endif
    auto* gpio = bflb_device_get_by_name("gpio");
    if (!gpio)
        return Result::unsupported;
    Guard guard;
    const auto result = slots.reserve(config, handle);
    if (result != Result::success)
        return result;
    auto& state = *slots.find(handle);
    const auto configured = detail::configure_input(state);
    if (configured != Result::success) {
        handle = {};
        return configured;
    }
    const auto pin = static_cast<std::uint8_t>(config.pin.value());
    auto trigger = config.edge == Edge::rising ? GPIO_INT_TRIG_MODE_SYNC_RISING_EDGE
                                               : GPIO_INT_TRIG_MODE_SYNC_FALLING_EDGE;
#if !defined(BL602) && !defined(BL702)
    if (config.edge == Edge::both)
        trigger = GPIO_INT_TRIG_MODE_SYNC_FALLING_RISING_EDGE;
#endif
    bflb_gpio_int_mask(gpio, pin, true);
    bflb_gpio_int_clear(gpio, pin);
    bflb_gpio_int_init(gpio, pin, trigger);
    bflb_gpio_irq_attach(pin, interrupt);
    bflb_gpio_int_mask(gpio, pin, false);
    bflb_irq_enable(gpio->irq_num);
    return Result::success;
}

Result window(Handle handle, const Window& config) {
    Guard guard;
    return detail::window(slots.find(handle), config);
}

Result snapshot(Handle handle, Snapshot& snapshot) {
    Guard guard;
    auto* state = slots.find(handle);
    if (!state || !state->active || state->faulted)
        return Result::invalid_argument;
    snapshot = state->captured;
    return Result::success;
}

Result close(Handle handle) {
    auto* gpio = bflb_device_get_by_name("gpio");
    if (!gpio)
        return Result::failed;
    Guard guard;
    auto* state = slots.find(handle);
    if (!state)
        return Result::success;
    const auto pin = static_cast<std::uint8_t>(state->configuration.pin.value());
    bflb_gpio_int_mask(gpio, pin, true);
    bflb_gpio_int_clear(gpio, pin);
    bflb_gpio_irq_detach(pin);
    *state = {};
    return Result::success;
}
} // namespace newmota::platform::edge_capture
