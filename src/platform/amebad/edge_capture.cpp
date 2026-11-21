/*
  edge_capture.cpp - AmebaD GPIO interrupt capture with exact 32768-Hz time conversion

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file edge_capture.cpp
 * @brief AmebaD GPIO interrupt capture with exact 32768-Hz time conversion.
 */

#include "newmota/platform/edge_capture_state.h"
extern "C" {
#include "ameba_soc.h"
}

namespace newmota::platform::edge_capture {
namespace {
detail::Slots slots;
bool registered_ports[2]{};

class Guard {
  public:
    Guard() : saved_(__get_PRIMASK()) { __disable_irq(); }

    ~Guard() { __set_PRIMASK(saved_); }

  private:
    std::uint32_t saved_;
};

std::uint32_t previous_tick{};
std::uint64_t extended_ticks{};

/** @brief Extends the native free-running 32-kHz timer; called each application iteration. */
std::uint64_t time_locked() {
    const auto tick = SYSTIMER_TickGet();
    extended_ticks += static_cast<std::uint32_t>(tick - previous_tick);
    previous_tick = tick;
    return (extended_ticks / 32768) * 1000000 + (extended_ticks % 32768) * 1000000 / 32768;
}

void interrupt(void* argument, std::uint32_t) {
    Guard guard;
    detail::record(*static_cast<detail::State*>(argument), time_locked());
}
} // namespace

std::uint64_t monotonic_microseconds() {
    Guard guard;
    return time_locked();
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
    GPIO_INTConfig(pin, DISABLE);
    auto* port = pin < 32 ? GPIOA_BASE : GPIOB_BASE;
    const auto irq = pin < 32 ? GPIOA_IRQ : GPIOB_IRQ;
    port->PORTA_EOI = std::uint32_t{1} << (pin % 32);
    GPIO_UserRegIrq(pin, reinterpret_cast<void*>(interrupt), &state);
    const unsigned index = pin < 32 ? 0 : 1;
    if (!registered_ports[index]) {
        InterruptRegister(GPIO_INTHandler, irq, reinterpret_cast<u32>(port), 5);
        InterruptEn(irq, 5);
        registered_ports[index] = true;
    }
    GPIO_INTMode(pin, ENABLE,
        config.edge == Edge::both ? GPIO_INT_Trigger_BOTHEDGE : GPIO_INT_Trigger_EDGE,
        config.edge == Edge::rising ? GPIO_INT_POLARITY_ACTIVE_HIGH : GPIO_INT_POLARITY_ACTIVE_LOW,
        GPIO_INT_DEBOUNCE_DISABLE);
    GPIO_INTConfig(pin, ENABLE);
    (void)time_locked();
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
    GPIO_INTConfig(pin, DISABLE);
    auto* port = pin < 32 ? GPIOA_BASE : GPIOB_BASE;
    port->PORTA_EOI = std::uint32_t{1} << (pin % 32);
    GPIO_UserRegIrq(pin, nullptr, nullptr);
    *state = {};
    return Result::success;
}
} // namespace newmota::platform::edge_capture
