/*
  edge_capture.cpp - Beken per-pin edge capture with atomic native snapshots

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file edge_capture.cpp
 * @brief Beken per-pin edge capture with atomic native snapshots.
 */

#include "newmota/platform/edge_capture_state.h"
#include "edge_capture_native.h"

namespace newmota::platform::edge_capture {
namespace {
detail::Slots slots;

class Guard {
  public:
    Guard() : saved_(newmota_beken_edge_lock()) {}

    ~Guard() { newmota_beken_edge_unlock(saved_); }

  private:
    std::uint32_t saved_;
};

void interrupt(unsigned char pin) {
    Guard guard;
    const auto now = newmota_beken_edge_time();
    for (auto& state : slots.states)
        if (state.active && state.configuration.pin.value() == pin)
            detail::record(state, now);
}
} // namespace

std::uint64_t monotonic_microseconds() {
    return newmota_beken_edge_time();
}

Result open(const Configuration& config, Handle& handle) {
    handle = {};
    // BK7231N/BK7238's two-bit trigger selector has no both-edge mode.
    if (config.edge == Edge::both)
        return Result::unsupported;
    if (config.pin.value() >= 32)
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
    newmota_beken_edge_start(config.pin.value(), config.edge == Edge::rising, interrupt);
    // The SDK registration chooses a pull from polarity. Restore the requested pull explicitly.
    if (gpio_configure_input(config.pin, config.pull) != GpioResult::success) {
        newmota_beken_edge_stop(config.pin.value());
        state.faulted = true;
        handle = {};
        return Result::failed;
    }
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
    newmota_beken_edge_stop(state->configuration.pin.value());
    *state = {};
    return Result::success;
}
} // namespace newmota::platform::edge_capture
