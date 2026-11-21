/*
  edge_capture_service.cpp - Exclusive application GPIO edge capture ownership

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file edge_capture_service.cpp
 * @brief Exclusive application GPIO edge capture ownership.
 */

#include "drivers/gpio/edge_capture_service.h"
#include "core/hardware/runtime_pin_claims.h"
#include <array>
#include <limits>

namespace newmota::edges {
namespace native = platform::edge_capture;

namespace {
struct State {
    native::Handle native_handle{};
    PinId pin{};
    std::uint32_t generation{};
    bool active{}, faulted{};
};

std::array<State, 16> states{};
std::uint32_t generation{1};

State* state(Handle handle) {
    if (handle.slot >= states.size())
        return nullptr;
    auto& entry = states[handle.slot];
    return entry.active && entry.generation == handle.generation ? &entry : nullptr;
}

runtime_pins::Owner token(std::uint32_t generation) {
    return {runtime_pins::OwnerKind::edge_capture, generation};
}
} // namespace

void poll() {
    (void)native::monotonic_microseconds();
}

Result open(const Configuration& configuration, Handle& handle) {
    handle = {};
    std::size_t slot{};
    while (slot < states.size() && (states[slot].active || states[slot].faulted))
        ++slot;
    if (slot == states.size() || generation == std::numeric_limits<std::uint32_t>::max())
        return Result::busy;
    const auto id = ++generation;
    if (!runtime_pins::claim(token(id), &configuration.pin, 1))
        return Result::busy;
    native::Handle native_handle{};
    const auto result = native::open(configuration, native_handle);
    if (result != Result::success) {
        if (result == Result::failed)
            states[slot].faulted = true;
        else
            runtime_pins::release(token(id));
        return result;
    }
    states[slot] = {native_handle, configuration.pin, id, true, false};
    handle = {id, static_cast<std::uint8_t>(slot)};
    return Result::success;
}

Result window(Handle handle, const Window& configuration) {
    auto* entry = state(handle);
    return entry && !entry->faulted ? native::window(entry->native_handle, configuration)
                                    : Result::invalid_argument;
}

Result snapshot(Handle handle, Snapshot& value) {
    auto* entry = state(handle);
    if (!entry || entry->faulted)
        return Result::invalid_argument;
    return native::snapshot(entry->native_handle, value);
}

Result close(Handle handle) {
    auto* entry = state(handle);
    if (!entry)
        return Result::success;
    const auto result = native::close(entry->native_handle);
    if (result != Result::success) {
        entry->faulted = true;
        return result;
    }
    runtime_pins::release(token(entry->generation));
    *entry = {};
    return Result::success;
}
} // namespace newmota::edges
