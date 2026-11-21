/*
  led_strip_service.cpp - Portable LED strip state and frame service

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file led_strip_service.cpp
 * @brief Portable LED strip state and frame service.
 */

#include "drivers/led_strip/led_strip.h"
#include "core/hardware/runtime_pin_claims.h"
#include "newmota/platform/gpio.h"
#include "newmota/platform/platform.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>

namespace newmota::led_strip {
namespace native = platform::led_strip;

namespace {
struct State {
    Config config{};
    Handle handle{};
    native::Port port{};
    Pixel* pixels{};
    std::uint8_t* wire{};
    void* allocation{};
    std::size_t capacity{};
    bool faulted{}, closing{}, pending{};
    void* editor_context{};
    Editor editor{};
    FaultHandler fault_handler{};
};

std::array<State, kCapacity> states{};
// Never reuse generations within this service.
std::uint32_t generation{1};
// The longest frame, with its reset, takes about 21 ms.
constexpr unsigned kIdleWaitMilliseconds = 50U;

constexpr runtime_pins::Owner pin_owner(const std::uint32_t instance) {
    return {runtime_pins::OwnerKind::led_strip, instance};
}

State* state(Handle h) {
    if (h.slot >= states.size() || !h.generation)
        return nullptr;
    auto& s = states[h.slot];
    return s.handle.generation == h.generation && !s.closing ? &s : nullptr;
}

void fault(State& s) {
    if (s.faulted)
        return;
    s.faulted = true;
    if (s.fault_handler)
        s.fault_handler(s.editor_context);
}

Result readiness(State& s) {
    if (s.faulted)
        return Result::failed;
    const auto result = native::readiness(s.port);
    if (result == Result::failed)
        fault(s);
    return result;
}

bool allocate(const std::size_t capacity, Pixel*& pixels, void*& allocation, std::uint8_t*& wire) {
    pixels = static_cast<Pixel*>(std::calloc(capacity, sizeof(Pixel)));
    // DMA adapters need at least 32-byte alignment. Keep the original pointer.
    // Extra tail padding also contains cache-line rounding by native cache clean.
    allocation = std::calloc(1, native::wire_size(capacity, true) + 63);
    if (!pixels || !allocation) {
        std::free(pixels);
        std::free(allocation);
        pixels = nullptr;
        allocation = nullptr;
        return false;
    }
    wire = reinterpret_cast<std::uint8_t*>(
        (reinterpret_cast<std::uintptr_t>(allocation) + 31) & ~std::uintptr_t{31});
    return true;
}
} // namespace

Result open(const Config& c, Handle& out) {
    out = {};
    if (!c.pin.valid() || !c.count || c.count > native::kMaximumPixels || unsigned(c.order) > 5)
        return Result::invalid_argument;
    for (const auto& s : states)
        if (s.handle.generation && s.config.managed && s.config.pin.value() == c.pin.value()) {
            if (s.faulted)
                return Result::failed;
            if (c.managed || c.rgbw != s.config.rgbw ||
                (c.rgbw && c.white_first != s.config.white_first) || c.count != s.config.count ||
                c.order != s.config.order || c.reverse != s.config.reverse)
                return Result::invalid_argument;
            out = s.handle;
            return Result::success;
        }
    if (!(platform::gpio_capabilities(c.pin) & capability_mask(PinCapability::output)))
        return Result::unsupported;
    std::size_t index{};
    while (index < states.size() && states[index].handle.generation)
        ++index;
    // Berry handles store generations in a positive signed 32-bit integer.
    if (index == states.size() ||
        generation >= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
        return Result::busy;
    const auto token = ++generation;
    // Designated GPIO_WS2812 pads use their own explicit binding owner path.
    if (!runtime_pins::claim_led_strip(pin_owner(token), c.pin))
        return Result::busy;
    Pixel* pixels{};
    void* allocation{};
    std::uint8_t* wire{};
    if (!allocate(c.count, pixels, allocation, wire)) {
        runtime_pins::release(pin_owner(token));
        return Result::no_memory;
    }
    native::Port port{};
    const auto result = native::open(c.pin, c.count, port);
    if (result != Result::success) {
        std::free(pixels);
        std::free(allocation);
        if (result == Result::failed)
            states[index] = {c, {token, static_cast<std::uint8_t>(index)}, port, nullptr, nullptr,
                nullptr, 0U, true};
        else
            runtime_pins::release(pin_owner(token));
        return result;
    }
    out = {token, static_cast<std::uint8_t>(index)};
    states[index] = {c, out, port, pixels, wire, allocation, c.count};
    return Result::success;
}

bool active(Handle h) {
    return state(h) != nullptr;
}

bool managed(Handle h) {
    const auto* s = state(h);
    return s && s->config.managed;
}

Result set_editor(Handle h, void* context, Editor editor, FaultHandler fault_handler) {
    auto* s = state(h);
    if (!s || !s->config.managed)
        return Result::invalid_argument;
    s->editor_context = context;
    s->editor = editor;
    s->fault_handler = fault_handler;
    return Result::success;
}

Result reserve(Handle h, const std::size_t count) {
    auto* s = state(h);
    if (!s || !s->config.managed || !count || count > native::kMaximumPixels)
        return Result::invalid_argument;
    if (s->faulted)
        return Result::failed;
    if (count <= s->capacity)
        return Result::success;
    auto idle = readiness(*s);
    for (unsigned waited = 0U; idle == Result::busy && waited < kIdleWaitMilliseconds; ++waited) {
        platform::delay_microseconds(1000U);
        idle = readiness(*s);
    }
    if (idle != Result::success)
        return idle;
    // The adapter buffer goes first, because it is the largest and the new one may need its room.
    if (const auto closed = native::close(s->port); closed != Result::success) {
        fault(*s);
        return closed;
    }
    Pixel* pixels{};
    void* allocation{};
    std::uint8_t* wire{};
    native::Port port{};
    const auto result = allocate(count, pixels, allocation, wire)
                            ? native::open(s->config.pin, count, port)
                            : Result::no_memory;
    if (result == Result::success) {
        std::copy_n(s->pixels, s->capacity, pixels);
        std::free(s->pixels);
        std::free(s->allocation);
        s->pixels = pixels;
        s->wire = wire;
        s->allocation = allocation;
        s->capacity = count;
        s->port = port;
        return result;
    }
    std::free(pixels);
    std::free(allocation);
    // The old buffers are still here, so the strip carries on at its old size.
    if (result != Result::failed &&
        native::open(s->config.pin, s->capacity, port) == Result::success) {
        s->port = port;
        return result;
    }
    fault(*s);
    return Result::failed;
}

Result configure(Handle h, const Config& config) {
    auto* s = state(h);
    if (!s || !s->config.managed || config.pin.value() != s->config.pin.value() || !config.count ||
        config.count > s->capacity || unsigned(config.order) > 5U)
        return Result::invalid_argument;
    if (s->faulted)
        return Result::failed;
    s->config = config;
    s->config.managed = true;
    return Result::success;
}

Result frame(Handle h, const Pixel* pixels, std::size_t count) {
    auto* s = state(h);
    if (!s || !s->config.managed || !pixels || count != s->config.count)
        return Result::invalid_argument;
    if (s->faulted)
        return Result::failed;
    for (std::size_t i = 0; i < count; ++i)
        s->pixels[i] = pixels[i];
    s->pending = true;
    const auto result = show(h);
    return result == Result::busy ? Result::success : result;
}

Result busy(Handle h, bool& out) {
    auto* s = state(h);
    if (!s)
        return Result::invalid_argument;
    const auto result = readiness(*s);
    out = result == Result::busy;
    return result == Result::busy ? Result::success : result;
}

Result set(Handle h, std::size_t index, Pixel pixel) {
    auto* s = state(h);
    if (!s || index >= s->config.count)
        return Result::invalid_argument;
    if (s->config.managed && s->editor)
        return s->editor(s->editor_context, index, pixel, 1U);
    if (const auto result = readiness(*s); result != Result::success)
        return result;
    s->pixels[index] = pixel;
    return Result::success;
}

Result fill(Handle h, Pixel pixel) {
    auto* s = state(h);
    if (!s)
        return Result::invalid_argument;
    if (s->config.managed && s->editor)
        return s->editor(s->editor_context, 0U, pixel, s->config.count);
    if (const auto result = readiness(*s); result != Result::success)
        return result;
    for (std::size_t i = 0; i < s->config.count; ++i)
        s->pixels[i] = pixel;
    return Result::success;
}

Result show(Handle h) {
    auto* s = state(h);
    if (!s)
        return Result::invalid_argument;
    if (const auto result = readiness(*s); result != Result::success)
        return result;
    const auto size = native::wire_size(s->config.count, s->config.rgbw);
    auto result = native::encode(s->pixels, s->config.count, s->config.order, s->config.reverse,
        s->wire, size, s->config.rgbw, s->config.white_first);
    if (result == Result::success)
        result = native::transmit(s->port, s->wire, size);
    if (result == Result::failed)
        fault(*s);
    if (result == Result::success)
        s->pending = false;
    return result;
}

Result close(Handle h) {
    if (h.slot >= states.size())
        return Result::invalid_argument;
    auto& s = states[h.slot];
    if (!h.generation || s.handle.generation != h.generation)
        return Result::success;
    if (s.config.managed)
        return s.faulted ? Result::failed : Result::success;
    if (s.faulted)
        return Result::failed;
    if (const auto result = readiness(s); result != Result::success)
        return result;
    const auto result = native::close(s.port);
    if (result != Result::success) {
        fault(s);
        return result;
    }
    std::free(s.pixels);
    std::free(s.allocation);
    runtime_pins::release(pin_owner(h.generation));
    s = {};
    return Result::success;
}

Result request_close(Handle h) {
    if (h.slot >= states.size())
        return Result::invalid_argument;
    auto& s = states[h.slot];
    if (s.handle.generation == h.generation && !s.config.managed)
        s.closing = true;
    return Result::success;
}

void poll() {
    for (auto& s : states) {
        if (!s.handle.generation || s.faulted || readiness(s) != Result::success)
            continue;
        if (s.closing)
            (void)close(s.handle);
        else if (s.pending)
            (void)show(s.handle);
    }
}
} // namespace newmota::led_strip
