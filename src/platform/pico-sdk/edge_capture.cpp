/*
  edge_capture.cpp - Pico native GPIO edge capture on the application core

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file edge_capture.cpp
 * @brief Pico native GPIO edge capture on the application core.
 */

#include "newmota/platform/edge_capture_state.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "pico/time.h"

namespace newmota::platform::edge_capture {
namespace {
detail::Slots slots;
std::uint64_t owned_mask{};

/** @brief All API calls and the registered raw handler belong to the application core. */
class Guard {
  public:
    Guard() : saved_(save_and_disable_interrupts()) {}

    ~Guard() { restore_interrupts(saved_); }

  private:
    std::uint32_t saved_;
};

std::uint32_t events(Edge edge) {
    return edge == Edge::rising    ? GPIO_IRQ_EDGE_RISE
           : edge == Edge::falling ? GPIO_IRQ_EDGE_FALL
                                   : GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL;
}

void interrupt() {
    Guard guard;
    const auto now = time_us_64();
    for (auto& state : slots.states) {
        if (!state.active || state.faulted)
            continue;
        const auto pin = state.configuration.pin.value();
        const auto pending = gpio_get_irq_event_mask(pin) & events(state.configuration.edge);
        if (pending) {
            gpio_acknowledge_irq(pin, pending);
            // A coalesced pair cannot be reconstructed as two timestamped edges.
            if (pending == (GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL))
                state.captured.overflow = true;
            detail::record(state, now);
        }
    }
}

/** @brief Keeps one shared raw handler while changing only this owner's pin mask. */
void change_mask(std::uint64_t mask) {
    if (owned_mask)
        gpio_remove_raw_irq_handler_masked64(owned_mask, interrupt);
    owned_mask = mask;
    if (owned_mask)
        gpio_add_raw_irq_handler_masked64(owned_mask, interrupt);
}
} // namespace

std::uint64_t monotonic_microseconds() {
    return time_us_64();
}

Result open(const Configuration& config, Handle& handle) {
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
    const auto pin = config.pin.value();
    gpio_set_irq_enabled(pin, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);
    gpio_acknowledge_irq(pin, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL);
    change_mask(owned_mask | (std::uint64_t{1} << pin));
    gpio_set_irq_enabled(pin, events(config.edge), true);
    irq_set_enabled(IO_IRQ_BANK0, true);
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
    Guard guard;
    auto* state = slots.find(handle);
    if (!state)
        return Result::success;
    const auto pin = state->configuration.pin.value();
    gpio_set_irq_enabled(pin, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);
    gpio_acknowledge_irq(pin, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL);
    // An input-configuration failure occurs before this slot joins the raw mask.
    if (owned_mask & (std::uint64_t{1} << pin))
        change_mask(owned_mask & ~(std::uint64_t{1} << pin));
    *state = {};
    return Result::success;
}
} // namespace newmota::platform::edge_capture
