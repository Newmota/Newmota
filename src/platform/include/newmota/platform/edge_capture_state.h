/*
  edge_capture_state.h - Shared native edge counter state and validation

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file edge_capture_state.h
 * @brief Shared native edge counter state and validation.
 */

#pragma once
#include "newmota/platform/edge_capture.h"
#include <array>
#include <cstddef>

namespace newmota::platform::edge_capture::detail {
/** @brief Native adapters must exclude their capture IRQs around every access to this state. */
struct State {
    Configuration configuration{};
    Snapshot captured{};
    std::uint32_t generation{};
    bool active{}, faulted{};
    Window window{};
};

/** @brief Fixed native slots; no protocol or per-driver allocation occurs in the ISR. */
class Slots {
  public:
    std::array<State, 16> states{};

    /** @brief Resolves an active or fault-retained slot by generation.
     * @param handle Native capture handle.
     * @return Matching state, or nullptr for a stale or invalid handle.
     */
    State* find(Handle handle) {
        if (handle.slot >= states.size())
            return nullptr;
        auto& state = states[handle.slot];
        return (state.active || state.faulted) && state.generation == handle.generation ? &state
                                                                                        : nullptr;
    }

    /** @brief Validates and reserves a distinct slot before native configuration.
     * @param config Pin, edge, pull, and debounce interval to retain.
     * @param handle Receives the generation-safe slot handle on success.
     * @return Validation/admission result; this does not configure GPIO or register an interrupt.
     */
    Result reserve(const Configuration& config, Handle& handle) {
        handle = {};
        if (!config.pin.valid() || unsigned(config.edge) > unsigned(Edge::both) ||
            unsigned(config.pull) > unsigned(GpioPull::down) ||
            (config.maximum_interval_us && config.maximum_interval_us < config.minimum_interval_us))
            return Result::invalid_argument;
        const auto requirements =
            capability_mask(PinCapability::input) | capability_mask(PinCapability::interrupt);
        if (!gpio_available(config.pin) ||
            (gpio_capabilities(config.pin) & requirements) != requirements)
            return Result::unsupported;
        for (const auto& state : states)
            if ((state.active || state.faulted) &&
                state.configuration.pin.value() == config.pin.value())
                return Result::busy;
        std::size_t index{};
        while (index < states.size() && (states[index].active || states[index].faulted))
            ++index;
        if (index == states.size() || generation_ == UINT32_MAX)
            return Result::busy;
        states[index] = {config, {}, ++generation_, true, false};
        handle = {generation_, static_cast<std::uint8_t>(index)};
        return Result::success;
    }

  private:
    std::uint32_t generation_{1};
};

/** @brief Updates one integer-only capture inside the adapter's interrupt exclusion guard.
 * @param state Active retained capture state protected by the native interrupt guard.
 * @param now Current monotonic microsecond timestamp.
 */
#if defined(__GNUC__)
__attribute__((always_inline))
#endif
inline void record(State& state, std::uint64_t now) {
    auto& sample = state.captured;
    if (!state.active || state.faulted ||
        (sample.seen && now - sample.last_edge_us < state.configuration.minimum_interval_us))
        return;
    ++sample.count;
    if (sample.seen) {
        const auto elapsed = now - sample.last_edge_us;
        const bool valid =
            elapsed <= UINT32_MAX && (!state.configuration.maximum_interval_us ||
                                         elapsed <= state.configuration.maximum_interval_us);
        const auto period = valid ? static_cast<std::uint32_t>(elapsed) : 0U;
        sample.last_period_us = period;
        if (valid) {
            ++sample.period_count;
            sample.summed_period_us += period;
            // A uint16_t limit of 32-bit periods cannot overflow the 64-bit window sum.
            const auto& window = state.window;
            if (window.limit && sample.window_count < window.limit && now >= window.begin_us &&
                now < window.end_us) {
                ++sample.window_count;
                sample.window_sum_us += period;
            }
        }
    }
    sample.last_edge_us = now;
    sample.seen = true;
}

/** @brief Rearms collection only while the native adapter excludes its capture interrupt. */
inline Result window(State* state, const Window& config) {
    if (!state || !state->active || state->faulted ||
        (config.limit && config.end_us <= config.begin_us))
        return Result::invalid_argument;
    state->window = config;
    state->captured.window_sum_us = 0;
    state->captured.window_count = 0;
    return Result::success;
}

/** @brief Initializes a reserved input and conservatively quarantines uncertain native failure.
 * @param state Previously reserved capture state.
 * @return success when configured; failed when ownership must remain quarantined; otherwise
 * unsupported when the native input was not initialized.
 */
inline Result configure_input(State& state) {
    const auto result = gpio_configure_input(state.configuration.pin, state.configuration.pull);
    if (result == GpioResult::success)
        return Result::success;
    if (result == GpioResult::failed) {
        state.faulted = true;
        return Result::failed;
    }
    state = {};
    return Result::unsupported;
}
} // namespace newmota::platform::edge_capture::detail
