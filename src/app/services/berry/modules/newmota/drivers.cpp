/*
  drivers.cpp - Tasmota-style Berry driver hooks

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/berry/modules/newmota/drivers.h"

#include "newmota/log.h"

extern "C" {
#include "be_gc.h"
#include "be_list.h"
#include "berry.h"
}

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

using newmota::BerryDriverEvent;
using newmota::BerryDriverHook;
using newmota::BerryDriverWord;

constexpr std::size_t kHookCount = static_cast<std::size_t>(BerryDriverHook::count);
constexpr std::array<const char*, kHookCount> kHookNames{"every_50ms", "every_100ms", "every_250ms",
    "every_second", "any_key", "switch_action", "button_pressed", "button_multi_pressed",
    "set_power_handler", "network_up", "network_down", "audio"};
constexpr std::array<const char*, 5> kWords{"", "power", "rate", "tx", "rx"};
constexpr std::uint8_t kTickHooks = 0x0FU;
// One Power event per output for a Power0 burst, plus room for input events.
constexpr std::size_t kInputCapacity = 40;

blist* driver_roots{}; ///< Rooted instances in registration order; grows like Tasmota's list.

std::array<BerryDriverEvent, kInputCapacity> inputs{};
std::size_t input_head{};
std::size_t input_count{};
std::size_t batch_inputs{}; ///< Inputs this poll may still deliver.
std::uint8_t pending_ticks{};
std::uint8_t tick_hooks{}; ///< Tick hooks a driver defined at the last refresh.
bool refresh_pending{};
BerryDriverHook network_pending{BerryDriverHook::count};
bool done{};

// Observations survive a VM restart; they describe the device, not the scripts.
bool power_seen{};
std::uint32_t last_power{};
bool last_online{};

struct AudioDirection {
    bool open{};
    std::uint32_t rate{};
};

AudioDirection last_tx{}, last_rx{};

/** @brief Reports whether any driver can receive an event; without one nothing is recorded. */
bool drivers_present() {
    return driver_roots != nullptr && be_list_count(driver_roots) != 0;
}

/** @brief Adds the tick hooks of the instance at stack @p index to tick_hooks. */
void add_tick_hooks(bvm* const vm, const int index) {
    for (unsigned hook = 0; hook < 4U; ++hook) {
        be_getmember(vm, index, kHookNames[hook]);
        if (be_isfunction(vm, -1))
            tick_hooks |= static_cast<std::uint8_t>(1U << hook);
        be_pop(vm, 1);
    }
}

/** @brief Queues the audio events of one direction's change since the last pass. */
void observe_audio_direction(AudioDirection& last, const bool open, const std::uint32_t rate,
    const BerryDriverWord direction) {
    if (open && (!last.open || rate != last.rate))
        newmota::queue_berry_driver_event(
            BerryDriverHook::audio, rate, BerryDriverWord::rate, direction);
    if (open != last.open)
        newmota::queue_berry_driver_event(
            BerryDriverHook::audio, open ? 1U : 0U, BerryDriverWord::power, direction);
    last = {open, open ? rate : 0U};
}

/** @brief Returns the slot of @p object, or -1. */
int find_driver(const void* const object) {
    for (int slot = 0; drivers_present() && slot < be_list_count(driver_roots); ++slot)
        if (var_toobj(be_list_at(driver_roots, slot)) == object)
            return slot;
    return -1;
}

} // namespace

namespace newmota {

void observe_berry_drivers(const std::uint8_t ticks, const std::uint32_t power, const bool online) {
    if (drivers_present()) {
        // Only ticks some driver handles wake the poll; a once-a-second refresh finds tick
        // methods assigned after add_driver().
        pending_ticks |= static_cast<std::uint8_t>(ticks & tick_hooks & kTickHooks);
        refresh_pending = refresh_pending || (ticks & (1U << 3)) != 0U;
    }
    if (!power_seen) {
        power_seen = true;
        last_power = power;
    } else if (power != last_power) {
        report_berry_driver_power(power);
    }
    if (online != last_online) {
        last_online = online;
        network_pending = !drivers_present() ? BerryDriverHook::count
                          : online           ? BerryDriverHook::network_up
                                             : BerryDriverHook::network_down;
    }
}

void report_berry_driver_power(const std::uint32_t power) {
    power_seen = true;
    last_power = power;
    queue_berry_driver_event(BerryDriverHook::set_power_handler, power);
}

void observe_berry_audio(
    const bool tx, const std::uint32_t tx_rate, const bool rx, const std::uint32_t rx_rate) {
    observe_audio_direction(last_tx, tx, tx_rate, BerryDriverWord::tx);
    observe_audio_direction(last_rx, rx, rx_rate, BerryDriverWord::rx);
}

void queue_berry_driver_event(const BerryDriverHook hook, const std::uint32_t idx,
    const BerryDriverWord cmd, const BerryDriverWord payload) {
    if (!drivers_present())
        return;
    if (input_count == inputs.size()) {
        AddLog(LogLevel::warning, "BRY: driver %s event dropped",
            kHookNames[static_cast<std::size_t>(hook)]);
        return;
    }
    inputs[(input_head + input_count++) % inputs.size()] = {hook, cmd, payload, idx};
}

bool berry_drivers_pending() {
    return input_count != 0U || network_pending != BerryDriverHook::count || pending_ticks != 0U ||
           refresh_pending;
}

void begin_berry_driver_batch() {
    batch_inputs = input_count;
}

bool next_berry_driver_event(BerryDriverEvent& event) {
    if (batch_inputs != 0U) {
        event = inputs[input_head];
        input_head = (input_head + 1U) % inputs.size();
        --input_count;
        --batch_inputs;
        return true;
    }
    if (network_pending != BerryDriverHook::count) {
        event = {network_pending, {}, {}, 0U};
        network_pending = BerryDriverHook::count;
        return true;
    }
    for (unsigned hook = 0; hook < 4U; ++hook) {
        if ((pending_ticks & (1U << hook)) != 0U) {
            pending_ticks &= static_cast<std::uint8_t>(~(1U << hook));
            event = {static_cast<BerryDriverHook>(hook), {}, {}, 0U};
            return true;
        }
    }
    return false;
}

std::size_t berry_driver_count() {
    return drivers_present() ? static_cast<std::size_t>(be_list_count(driver_roots)) : 0U;
}

bool berry_driver_refresh_pending() {
    return refresh_pending;
}

int refresh_berry_driver_hooks(bvm* const vm) {
    refresh_pending = false;
    tick_hooks = 0U;
    for (int slot = 0; slot < static_cast<int>(berry_driver_count()); ++slot) {
        be_pushnil(vm);
        var_setval(be_indexof(vm, -1), be_list_at(driver_roots, slot));
        add_tick_hooks(vm, be_absindex(vm, -1));
        be_pop(vm, 1);
    }
    be_return_nil(vm);
}

bool berry_driver_done() {
    return done;
}

int dispatch_berry_driver(bvm* const vm) {
    const auto* event = static_cast<const BerryDriverEvent*>(be_tocomptr(vm, 1));
    const auto slot = static_cast<int>(be_toint(vm, 2));
    done = false;
    if (event == nullptr || slot >= static_cast<int>(berry_driver_count()))
        be_return_nil(vm);
    be_pushnil(vm);
    var_setval(be_indexof(vm, -1), be_list_at(driver_roots, slot));
    const int instance = be_absindex(vm, -1);
    // Duck typed on every event, as Tasmota's introspect.get(): a method or any member that
    // holds a function, including one assigned after add_driver().
    be_getmember(vm, instance, kHookNames[static_cast<std::size_t>(event->hook)]);
    const int function = be_absindex(vm, -1);
    if (!be_isfunction(vm, function))
        be_return_nil(vm);
    // Tasmota's driver call: f(self, cmd, idx, payload) with the event's words; raw stays nil.
    be_pushvalue(vm, instance);
    be_pushstring(vm, kWords[static_cast<std::size_t>(event->cmd)]);
    be_pushint(vm, static_cast<bint>(static_cast<std::int32_t>(event->idx)));
    be_pushstring(vm, kWords[static_cast<std::size_t>(event->payload)]);
    be_call(vm, 4);
    done = be_tobool(vm, function);
    be_return_nil(vm);
}

int add_berry_driver(bvm* const vm) {
    if (be_top(vm) != 1 || !be_isinstance(vm, 1))
        be_raise(vm, "value_error", "instance required");
    if (find_driver(var_toobj(be_indexof(vm, 1))) >= 0)
        be_return_nil(vm); // added only once, as in Tasmota
    if (driver_roots == nullptr) {
        auto* list = be_list_new(vm);
        be_pushnil(vm);
        var_setlist(be_indexof(vm, -1), list);
        be_gc_fix(vm, reinterpret_cast<bgcobject*>(list));
        be_pop(vm, 1);
        driver_roots = list;
    }
    (void)be_list_push(vm, driver_roots, be_indexof(vm, 1));
    add_tick_hooks(vm, 1);
    be_return_nil(vm);
}

int remove_berry_driver(bvm* const vm) {
    if (be_top(vm) != 1 || !be_isinstance(vm, 1))
        be_return_nil(vm);
    const int slot = find_driver(var_toobj(be_indexof(vm, 1)));
    if (slot >= 0) {
        (void)be_list_remove(vm, driver_roots, slot); // later drivers keep their order
        refresh_pending = true;                       // its tick hooks may be the last ones
    }
    be_return_nil(vm);
}

void remove_berry_drivers(bvm* const vm) {
    if (driver_roots != nullptr)
        be_gc_unfix(vm, reinterpret_cast<bgcobject*>(driver_roots));
    driver_roots = nullptr;
    input_head = input_count = batch_inputs = 0U;
    pending_ticks = tick_hooks = 0U;
    refresh_pending = false;
    network_pending = BerryDriverHook::count;
    done = false;
}

} // namespace newmota
