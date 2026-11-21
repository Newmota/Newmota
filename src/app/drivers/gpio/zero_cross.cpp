/*
  zero_cross.cpp - Shared zero-cross relay-write timing

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Timing algorithm reference: Tasmota support_tasmota.ino ZeroCrossMomentStart/End
  Copyright (C) 2021 Theo Arends and Tasmota contributors
*/

/** @file zero_cross.cpp
 * @brief Predicts the compensated relay trigger from atomic native edge snapshots.
 */
#include "drivers/gpio/zero_cross.h"
#include "drivers/gpio/edge_capture_service.h"
#include "newmota/platform/platform.h"
#include <array>

namespace newmota::zero_cross {
namespace {
struct Route {
    edges::Handle capture{};
    std::array<PinId, 2> relays{};
    std::uint64_t write_started{};
    std::uint32_t generation{}, compensation{}, code_offset{}, missed{}, lateness{};
    bool active{};
};

std::array<Route, 8> routes{};
const Hardware* hardware{};
std::uint32_t generation{};
std::uint32_t group_depth{};
std::array<Handle, 8> group_handles{};

Route* route(Handle handle) {
    if (handle.slot >= routes.size())
        return nullptr;
    auto& result = routes[handle.slot];
    return result.generation == handle.generation && result.active ? &result : nullptr;
}

bool fresh(Route& value, edges::Snapshot& snapshot) {
    if (edges::snapshot(value.capture, snapshot) != edges::Result::success || snapshot.overflow ||
        !snapshot.seen || snapshot.last_period_us < 4000 || snapshot.last_period_us > 20000)
        return false;
    const auto now = platform::edge_capture::monotonic_microseconds();
    return now >= snapshot.last_edge_us &&
           now - snapshot.last_edge_us <= 2ULL * snapshot.last_period_us;
}
} // namespace

void initialize(const Hardware& configured) {
    hardware = &configured;
}

bool open(PinId input, const PinId* relays, std::size_t count, std::uint32_t compensation,
    Handle& handle) {
    handle = {};
    if (!hardware || !relays || !count || count > 2 || compensation > 20000 ||
        generation == UINT32_MAX)
        return false;
    std::size_t slot{};
    while (slot < routes.size() && routes[slot].generation)
        ++slot;
    if (slot == routes.size())
        return false;
    for (std::size_t i = 0; i < count; ++i) {
        const auto* binding = hardware->find_pin(relays[i]);
        if (!binding || binding->role != PinRole::relay)
            return false;
        for (std::size_t j = 0; j < i; ++j)
            if (relays[j].value() == relays[i].value())
                return false;
        for (const auto& prior : routes)
            if (prior.generation)
                for (const auto pin : prior.relays)
                    if (pin.valid() && pin.value() == relays[i].value())
                        return false;
    }
    edges::Configuration config{};
    config.pin = input;
    config.edge = platform::edge_capture::Edge::both;
    config.pull = platform::GpioPull::up;
    auto& value = routes[slot];
    if (edges::open(config, value.capture) != edges::Result::success)
        return false;
    value.generation = ++generation;
    value.compensation = compensation;
    value.active = true;
    for (std::size_t i = 0; i < count; ++i)
        value.relays[i] = relays[i];
    handle = {value.generation, static_cast<std::uint8_t>(slot)};
    return true;
}

void close(Handle handle) {
    auto* value = route(handle);
    if (!value)
        return;
    if (edges::close(value->capture) == edges::Result::success)
        *value = {};
    else
        value->active = false; // Keep the mapping reserved if native cleanup is uncertain.
}

Status status(Handle handle) {
    auto* value = route(handle);
    if (!value)
        return {};
    edges::Snapshot snapshot{};
    const bool valid = fresh(*value, snapshot);
    return {valid, snapshot.last_period_us, value->missed, value->lateness};
}

WriteGroup::WriteGroup() {
    ++group_depth;
}

WriteGroup::~WriteGroup() {
    if (!group_depth || --group_depth)
        return;
    next_window();
}

void next_window() {
    for (auto& handle : group_handles) {
        end(handle);
        handle = {};
    }
}

Handle begin(PinId relay) {
    for (std::size_t slot = 0; slot < routes.size(); ++slot) {
        auto& value = routes[slot];
        if (!value.active)
            continue;
        bool matches{};
        for (const auto pin : value.relays)
            matches |= pin.valid() && pin.value() == relay.value();
        if (!matches)
            continue;
        const Handle handle{value.generation, static_cast<std::uint8_t>(slot)};
        if (group_depth && group_handles[slot].generation == handle.generation)
            return {}; // The enclosing operation already synchronized this source.
        if (group_depth)
            group_handles[slot] = handle;
        edges::Snapshot snapshot{};
        if (fresh(value, snapshot)) {
            const auto now = platform::edge_capture::monotonic_microseconds();
            const auto offset = std::uint64_t(value.compensation) + value.code_offset;
            const auto period = snapshot.last_period_us;
            const auto cycles = (now + offset - snapshot.last_edge_us) / period + 1;
            const auto target = snapshot.last_edge_us + cycles * period - offset;
            if (target > now && target - now <= period) {
                platform::delay_microseconds(static_cast<std::uint32_t>(target - now));
                const auto actual = platform::edge_capture::monotonic_microseconds();
                value.lateness = actual > target ? static_cast<std::uint32_t>(actual - target) : 0;
                value.write_started = actual;
                return group_depth ? Handle{} : handle;
            }
        }
        if (value.missed != UINT32_MAX)
            ++value.missed;
        value.write_started = platform::edge_capture::monotonic_microseconds();
        return group_depth ? Handle{} : handle;
    }
    return {};
}

void end(Handle handle) {
    auto* value = route(handle);
    if (!value)
        return;
    const auto elapsed = platform::edge_capture::monotonic_microseconds() - value->write_started;
    value->code_offset = elapsed < 2000 ? static_cast<std::uint32_t>(elapsed / 2) : 1000;
}
} // namespace newmota::zero_cross
