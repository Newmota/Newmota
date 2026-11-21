/*
  adc_service.cpp - Fair configured ADC sampling owner

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file adc_service.cpp
 * @brief Fair configured ADC sampling owner.
 */

#include "drivers/adc/adc_service.h"
#include "core/hardware/runtime_pin_claims.h"
#include <array>

namespace newmota::adc {
namespace {
struct Input {
    runtime_pins::Owner owner{};
    PinId pin{};
    std::uint32_t generation{}, sum_mv{}, sum_raw{}, window_at{};
    std::uint8_t samples{};
    Snapshot snapshot{};
    bool active{}, fault{};
};

std::array<Input, kMaximumInputs> inputs;
std::uint32_t generation{}, started{}, last_start{};
std::size_t next{}, pending{kMaximumInputs};
bool started_once{};

Input* find(Handle handle) {
    if (handle.slot >= inputs.size())
        return nullptr;
    auto& input = inputs[handle.slot];
    return input.active && input.generation == handle.generation ? &input : nullptr;
}

/** @brief Drops the entire partial window; stale completed data is explicitly invalid. */
void fail(Input& input) {
    input.samples = 0;
    input.sum_mv = input.sum_raw = 0;
    input.snapshot.valid = false;
    ++input.snapshot.errors;
}
} // namespace

Result open(PinId pin, Handle& handle) {
    handle = {};
    if (!pin.valid() || generation == UINT32_MAX)
        return Result::invalid_argument;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        auto& input = inputs[i];
        if (input.active)
            continue;
        const runtime_pins::Owner owner{runtime_pins::OwnerKind::adc, ++generation};
        if (!runtime_pins::claim(owner, &pin, 1))
            return Result::busy;
        input = {};
        input.owner = owner;
        input.pin = pin;
        input.generation = generation;
        input.active = true;
        handle = {generation, static_cast<std::uint8_t>(i)};
        const auto result = platform::adc::open(pin);
        if (result == Result::success)
            return result;
        if (result == Result::failed && platform::adc::close(pin) != Result::success) {
            input.fault = true;
            return Result::failed;
        }
        runtime_pins::release(owner);
        input = {};
        handle = {};
        return result;
    }
    return Result::busy;
}

Result close(Handle handle) {
    auto* input = find(handle);
    if (!input)
        return Result::success;
    if (pending == handle.slot) {
        if (platform::adc::cancel() != Result::success) {
            input->fault = true;
            return Result::failed;
        }
        pending = kMaximumInputs;
    }
    if (platform::adc::close(input->pin) != Result::success) {
        input->fault = true;
        return Result::failed;
    }
    runtime_pins::release(input->owner);
    *input = {};
    return Result::success;
}

Result snapshot(Handle handle, Snapshot& output) {
    const auto* input = find(handle);
    if (!input || input->fault)
        return Result::invalid_argument;
    output = input->snapshot;
    return Result::success;
}

void poll(std::uint32_t now) {
    if (pending < inputs.size()) {
        auto& input = inputs[pending];
        platform::adc::Reading reading;
        const auto result = now - started >= 10 ? Result::failed : platform::adc::finish(reading);
        if (result == Result::busy)
            return;
        if (result != Result::success) {
            fail(input);
            if (platform::adc::cancel() != Result::success) {
                input.fault = true;
                return;
            }
        } else {
            // The native millivolt range is bounded independently of reference provenance.
            if (reading.millivolts > 65535 || reading.raw > UINT32_MAX / 32)
                fail(input);
            else {
                if (input.samples && now - input.window_at > 2000)
                    fail(input);
                if (!input.samples)
                    input.window_at = now;
                input.sum_mv += reading.millivolts;
                input.sum_raw += reading.raw;
                if (++input.samples == 32) {
                    input.snapshot.millivolts = input.sum_mv / 32;
                    input.snapshot.raw = input.sum_raw / 32;
                    input.snapshot.completed_at = now;
                    input.snapshot.reference = reading.reference;
                    input.snapshot.valid = true;
                    ++input.snapshot.sequence;
                    input.samples = 0;
                    input.sum_mv = input.sum_raw = 0;
                }
            }
        }
        pending = kMaximumInputs;
    }
    if (started_once && now - last_start < 1)
        return;
    for (std::size_t visited = 0; visited < inputs.size(); ++visited) {
        const auto index = next;
        next = (next + 1) % inputs.size();
        auto& input = inputs[index];
        if (!input.active || input.fault)
            continue;
        const auto result = platform::adc::start(input.pin);
        last_start = now;
        started_once = true;
        if (result == Result::success) {
            pending = index;
            started = now;
        } else if (result != Result::busy)
            fail(input);
        return;
    }
}
} // namespace newmota::adc
