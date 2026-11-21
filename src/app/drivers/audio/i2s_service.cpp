/*
  i2s_service.cpp - Shared I2S transport lease and pad ownership

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file i2s_service.cpp
 * @brief Acquires each endpoint transactionally and protects stale handles.
 */
#include "drivers/audio/i2s_service.h"
#include "core/hardware/runtime_pin_claims.h"
#include "newmota/platform/gpio.h"
#include <array>
#include <limits>

namespace newmota {
namespace {
const Hardware* i2s_hardware{};
}

void initialize_i2s(const Hardware& h) {
    i2s_hardware = &h;
}
} // namespace newmota

namespace newmota::i2s {
namespace native = platform::i2s;

namespace {
constexpr std::size_t kMaximumOpenEndpoints = 8;
constexpr std::size_t kI2sSignalCount = 5;
constexpr std::size_t kProfilePadCount = 2 * kI2sSignalCount;
constexpr std::int8_t kAnyController = -1;
constexpr std::uint8_t kMinimumSlotCount = 1;
constexpr std::uint8_t kMaximumSlotCount = 16;

struct State {
    std::uint32_t generation{};
    native::Port port{};
    Config config{};
    std::array<PinId, kProfilePadCount> pads{};
    std::size_t pad_count{};
    std::int8_t controller{-1};
    bool occupied{}, faulted{}, native_open{};
};

std::array<State, kMaximumOpenEndpoints> endpoints{};
std::uint32_t generation{1};

constexpr runtime_pins::Owner owner(std::uint32_t token) {
    return {runtime_pins::OwnerKind::audio, token};
}

/** @brief Resolves a healthy lease. Capture may read until its worker is joined. */
State* state(Handle h) {
    if (h.slot >= endpoints.size())
        return nullptr;
    auto& s = endpoints[h.slot];
    return s.occupied && !s.faulted && s.generation == h.generation ? &s : nullptr;
}
} // namespace

bool configured_config(Config& c, native::Direction direction, std::uint8_t instance) {
    if (!i2s_hardware)
        return false;
    c = {};
    auto assigned = [instance](PinRole role) {
        const auto* b = i2s_hardware->find_function(role, instance + 1);
        return b && b->active() ? b->pin : PinId{};
    };
    if (direction == native::Direction::transmit)
        c.dout = assigned(PinRole::i2s_dout);
    else
        c.din = assigned(PinRole::i2s_din);
    c.bclk = assigned(PinRole::i2s_bclk);
    c.ws = assigned(PinRole::i2s_ws);
    c.mclk = assigned(PinRole::i2s_mclk);
    // Mode and controller/target role are explicit configuration, never pin inference.
    return c.dout.valid() || c.din.valid();
}

namespace {
Result validate(const Config& c) {
    if (!native::valid_mode(c.mode) || !native::valid_framing(c.framing) ||
        !native::valid_role(c.role) ||
        (c.msb_right != native::MsbRight::automatic && c.msb_right != native::MsbRight::enabled &&
            c.msb_right != native::MsbRight::disabled) ||
        c.controller < kAnyController || (!c.dout.valid() && !c.din.valid()) ||
        (c.mode == native::Mode::pdm ? !native::pdm_clock_pin(c).valid()
                                     : (!c.bclk.valid() || !c.ws.valid())) ||
        !c.sample_rate ||
        c.sample_rate > (c.mode == native::Mode::pdm && !c.pdm_pcm ? native::kMaximumRawPdmRateHz
                                                                   : native::kMaximumPcmRateHz) ||
        !native::valid_sample_width(c.sample_bits) || !native::valid_sample_width(c.slot_bits) ||
        native::sample_width_bits(c.sample_bits) > native::sample_width_bits(c.slot_bits) ||
        c.slots < kMinimumSlotCount || c.slots > kMaximumSlotCount || !c.slot_mask ||
        c.slot_mask >> c.slots ||
        (c.mode != native::Mode::tdm && c.slots != native::kStandardSlotCount) || !c.dma_frames ||
        c.dma_frames > native::kMaximumDmaFrames ||
        c.tuning.dma_descriptors < native::kMinimumDmaDescriptors ||
        c.tuning.dma_descriptors > native::kMaximumDmaDescriptors)
        return Result::invalid_argument;
    const auto caps = native::capabilities();
    const native::Tuning defaults{};
    if ((!caps.tuning && !(c.tuning == defaults)) || (c.tuning.apll && !caps.apll) ||
        (!caps.serial_order && (c.tuning.left_align != defaults.left_align || c.tuning.big_endian ||
                                   c.tuning.lsb_first)))
        return Result::unsupported;
    if (!(caps.modes & native::mode_mask(c.mode)) || (c.din.valid() && !caps.rx) ||
        (c.dout.valid() && !caps.tx) || (c.din.valid() && c.dout.valid() && !caps.duplex) ||
        (c.mode == native::Mode::pdm &&
            ((c.din.valid() && !caps.pdm_rx) || (c.dout.valid() && !caps.pdm_tx))))
        return Result::unsupported;
    return Result::success;
}

/** @brief Collects profile signals, permitting only matching roles on shared profile pads. */
Result collect(const Config& c, State& lease, std::array<PinRole, kProfilePadCount>& roles) {
    if (!c.din.valid() && !c.dout.valid())
        return Result::success;
    const bool pdm = c.mode == native::Mode::pdm;
    const std::array<PinId, kI2sSignalCount> requested{pdm ? native::pdm_clock_pin(c) : c.bclk,
        pdm ? PinId{} : c.ws, c.dout, c.din, pdm ? PinId{} : c.mclk};
    std::array<PinRole, kI2sSignalCount> requested_roles{pdm ? PinRole::i2s_ws : PinRole::i2s_bclk,
        PinRole::i2s_ws, PinRole::i2s_dout, PinRole::i2s_din, PinRole::i2s_mclk};
    // An explicit PDM clock can reuse either assigned clock route, independent of role.
    if (pdm && i2s_hardware) {
        const auto* binding = i2s_hardware->find_pin(requested[0]);
        if (binding && binding->role == PinRole::i2s_bclk)
            requested_roles[0] = PinRole::i2s_bclk;
    }
    for (std::size_t i = 0; i < requested.size(); ++i) {
        const auto pad = requested[i];
        if (!pad.valid())
            continue;
        for (std::size_t j = 0; j < i; ++j)
            if (requested[j].valid() && requested[j].value() == pad.value())
                return Result::invalid_argument;
        const bool input =
            requested_roles[i] == PinRole::i2s_din || (c.role == native::Role::target && i < 2);
        if (!(platform::gpio_capabilities(pad) &
                capability_mask(input ? PinCapability::input : PinCapability::output)))
            return Result::unsupported;
        bool present{};
        for (std::size_t j = 0; j < lease.pad_count; ++j) {
            if (lease.pads[j].value() != pad.value())
                continue;
            if (roles[j] != requested_roles[i])
                return Result::invalid_argument;
            present = true;
        }
        if (!present) {
            lease.pads[lease.pad_count] = pad;
            roles[lease.pad_count++] = requested_roles[i];
        }
    }
    return Result::success;
}
} // namespace

Result reserve(const Config& input, const Config& output, Handle& h) {
    h = {};
    if ((input.controller >= 0 && output.controller >= 0 &&
            input.controller != output.controller) ||
        input.controller < kAnyController || output.controller < kAnyController)
        return Result::invalid_argument;
    const auto caps = native::capabilities();
    if (!caps.controllers)
        return Result::unsupported;
    std::size_t index{};
    while (index < endpoints.size() && endpoints[index].occupied)
        ++index;
    if (index == endpoints.size() ||
        generation >= std::uint32_t(std::numeric_limits<std::int32_t>::max()))
        return Result::busy;
    State lease{};
    std::array<PinRole, kProfilePadCount> roles{};
    auto result = collect(input, lease, roles);
    if (result == Result::success)
        result = collect(output, lease, roles);
    if (result != Result::success)
        return result;
    if (!lease.pad_count)
        return Result::invalid_argument;
    auto selected = input.controller >= 0 ? input.controller : output.controller;
    if (selected >= caps.controllers)
        return Result::unsupported;
    // ESP PCM/PDM converters require controller zero. Other backends reject PDM
    // when activating it; keeping an unsupported inactive profile is harmless.
    if (selected < 0 && ((input.mode == native::Mode::pdm && input.pdm_pcm) ||
                            (output.mode == native::Mode::pdm && output.pdm_pcm)))
        selected = 0;
    for (std::uint8_t candidate = 0; candidate < caps.controllers; ++candidate) {
        if (selected >= 0 && candidate != selected)
            continue;
        bool occupied{};
        for (const auto& other : endpoints)
            occupied = occupied || (other.occupied && other.controller == candidate);
        if (!occupied) {
            lease.controller = candidate;
            break;
        }
    }
    if (lease.controller < 0)
        return Result::busy;
    const auto token = ++generation;
    if (!runtime_pins::claim_i2s(owner(token), lease.pads.data(), roles.data(), lease.pad_count))
        return Result::busy;
    lease.generation = token;
    lease.occupied = true;
    endpoints[index] = lease;
    h = {token, static_cast<std::uint8_t>(index)};
    return Result::success;
}

Result deactivate(Handle h) {
    auto* s = state(h);
    if (!s)
        return Result::invalid_argument;
    if (!s->native_open)
        return Result::success;
    const auto result = native::close(s->port);
    if (result != Result::success) {
        s->faulted = true;
        return result;
    }
    s->native_open = false;
    return Result::success;
}

Result check_profiles(Handle h, const Config& input, const Config& output) {
    const auto* s = state(h);
    if (!s)
        return Result::invalid_argument;
    State profiles{};
    std::array<PinRole, kProfilePadCount> roles{};
    for (const auto* config : {&input, &output}) {
        if (config->controller >= 0 && config->controller != s->controller)
            return Result::invalid_argument;
        const auto result = collect(*config, profiles, roles);
        if (result != Result::success)
            return result;
    }
    for (std::size_t i = 0; i < profiles.pad_count; ++i) {
        bool reserved{};
        for (std::size_t j = 0; j < s->pad_count; ++j)
            reserved = reserved || profiles.pads[i].value() == s->pads[j].value();
        if (!reserved)
            return Result::invalid_argument;
    }
    return Result::success;
}

Result activate(Handle h, const Config& requested) {
    auto* s = state(h);
    if (!s)
        return Result::invalid_argument;
    auto config = requested;
    if (config.controller >= 0 && config.controller != s->controller)
        return Result::invalid_argument;
    config.controller = s->controller;
    auto result = validate(config);
    if (result != Result::success)
        return result;
    result = check_profiles(h, config, {});
    if (result != Result::success)
        return result;
    result = deactivate(h);
    if (result != Result::success)
        return result;
    result = native::open(config, s->port);
    s->config = config;
    s->native_open = result == Result::success || result == Result::failed;
    s->faulted = result == Result::failed;
    return result;
}

Result open(const Config& config, Handle& h) {
    const auto checked = validate(config);
    if (checked != Result::success)
        return checked;
    auto result = reserve(config, {}, h);
    if (result == Result::success)
        result = activate(h, config);
    if (result != Result::success && result != Result::failed && h.generation) {
        (void)close(h);
        h = {};
    }
    return result;
}

Result close(Handle h) {
    if (h.slot >= endpoints.size())
        return Result::invalid_argument;
    auto& s = endpoints[h.slot];
    if (!s.occupied || s.generation != h.generation)
        return Result::success;
    const auto result = s.native_open ? native::close(s.port) : Result::success;
    if (result != Result::success) {
        s.faulted = true;
        return result;
    }
    runtime_pins::release(owner(s.generation));
    s = {};
    return Result::success;
}

bool active(Handle h) {
    return state(h) != nullptr;
}

bool tx_idle(Handle h) {
    auto* s = state(h);
    return s && s->native_open && native::tx_idle(s->port);
}

Transfer read(Handle h, std::uint8_t* data, std::size_t size) {
    auto* s = state(h);
    if (!s || !s->native_open || !s->config.din.valid() || (!data && size) ||
        size % native::frame_bytes(s->config))
        return {Result::invalid_argument, 0};
    return size ? native::read(s->port, data, size) : Transfer{};
}

Transfer write(Handle h, const std::uint8_t* data, std::size_t size) {
    auto* s = state(h);
    if (!s || !s->native_open || !s->config.dout.valid() || (!data && size) ||
        size % native::frame_bytes(s->config))
        return {Result::invalid_argument, 0};
    return size ? native::write(s->port, data, size) : Transfer{};
}

Status status(Handle h) {
    auto* s = state(h);
    return s && s->native_open ? native::status(s->port) : Status{};
}

Activity activity() {
    Activity a{};
    for (const auto& s : endpoints) {
        if (!s.occupied || !s.native_open)
            continue;
        if (s.config.dout.valid())
            a = {true, a.rx, s.config.sample_rate, a.rx_rate};
        if (s.config.din.valid())
            a = {a.tx, true, a.tx_rate, s.config.sample_rate};
    }
    return a;
}

void poll() {
    native::poll();
}
} // namespace newmota::i2s
