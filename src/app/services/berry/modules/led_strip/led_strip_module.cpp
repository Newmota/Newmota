/*
  led_strip_module.cpp - Berry LED strip module bindings

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file led_strip_module.cpp
 * @brief Berry LED strip module bindings.
 */

#include "services/berry/modules/led_strip/led_strip_module.h"
#include "drivers/led_strip/led_strip.h"
#include <array>
#include <cstring>
extern "C" {
#include "berry.h"
}

namespace {
namespace strip = newmota::led_strip;
std::array<strip::Handle, strip::kCapacity> handles{};

int integer(bvm* vm, int at, int low, int high) {
    if (!be_isint(vm, at))
        be_raise(vm, "type_error", "led_strip expects integer arguments");
    const auto value = be_toint(vm, at);
    if (value < low || value > high)
        be_raise(vm, "value_error", "led_strip argument out of range");
    return static_cast<int>(value);
}

void check(bvm* vm, strip::Result result) {
    if (result == strip::Result::success)
        return;
    if (result == strip::Result::busy)
        be_raise(vm, "runtime_error", "led_strip is busy or its pad is reserved");
    if (result == strip::Result::unsupported)
        be_raise(vm, "value_error", "unsupported led_strip hardware route");
    if (result == strip::Result::invalid_argument)
        be_raise(vm, "value_error", "invalid led_strip argument");
    if (result == strip::Result::no_memory)
        be_raise(vm, "memory_error", "led_strip allocation failed");
    be_raise(vm, "runtime_error", "led_strip hardware failure; ownership retained");
}

strip::Handle handle(bvm* vm) {
    be_getmember(vm, 1, ".generation");
    const auto generation = be_isint(vm, -1) ? static_cast<std::uint32_t>(be_toint(vm, -1)) : 0;
    be_pop(vm, 1);
    for (auto& h : handles)
        if (generation && h.generation == generation) {
            if (strip::active(h))
                return h;
            h = {};
            break;
        }
    return {};
}

strip::Pixel pixel(bvm* vm, int at) {
    if (!be_isint(vm, at))
        be_raise(vm, "type_error", "led_strip expects an integer color");
    const auto value = be_toint(vm, at);
    if (value < (-2147483647LL - 1LL) ||
        (value >= 0 && static_cast<std::uint64_t>(value) > 0xffffffffULL))
        be_raise(vm, "value_error", "led_strip color out of range");
    const auto color = static_cast<std::uint32_t>(value);
    return {static_cast<std::uint8_t>(color >> 16), static_cast<std::uint8_t>(color >> 8),
        static_cast<std::uint8_t>(color), static_cast<std::uint8_t>(color >> 24)};
}

int Init(bvm* vm) {
    if (be_top(vm) < 3 || be_top(vm) > 5)
        be_raise(vm, "type_error", "led_strip(pin, count[, order, reverse])");
    if (handle(vm).generation)
        be_raise(vm, "runtime_error", "close led_strip before reinitializing");
    strip::Config config{};
    config.pin = newmota::PinId{static_cast<std::uint16_t>(integer(vm, 2, 0, 65534))};
    config.count = static_cast<std::size_t>(
        integer(vm, 3, 1, static_cast<int>(newmota::platform::led_strip::kMaximumPixels)));
    if (be_top(vm) >= 4) {
        if (!be_isstring(vm, 4))
            be_raise(vm, "type_error", "led_strip order expects a string");
        const char* orders[] = {"RGB", "GRB", "RBG", "GBR", "BRG", "BGR"};
        const auto* text = be_tostring(vm, 4);
        char rgb[4]{};
        const auto length = std::strlen(text);
        if (length == 4U && (text[0] == 'W' || text[3] == 'W')) {
            config.rgbw = true;
            config.white_first = text[0] == 'W';
            std::memcpy(rgb, text + (config.white_first ? 1 : 0), 3);
        } else if (length == 3U) {
            std::memcpy(rgb, text, 3);
        } else {
            be_raise(vm, "value_error", "invalid led_strip color order");
        }
        unsigned order{};
        while (order < 6 && std::strcmp(rgb, orders[order]))
            ++order;
        if (order == 6)
            be_raise(vm, "value_error", "invalid led_strip color order");
        config.order = static_cast<strip::Order>(order);
    }
    if (be_top(vm) == 5) {
        if (!be_isbool(vm, 5))
            be_raise(vm, "type_error", "led_strip reverse expects bool");
        config.reverse = be_tobool(vm, 5);
    }
    strip::Handle opened{};
    check(vm, strip::open(config, opened));
    handles[opened.slot] = opened;
    be_pushint(vm, static_cast<bint>(opened.generation));
    be_setmember(vm, 1, ".generation");
    be_pop(vm, 1);
    be_return_nil(vm);
}

int Set(bvm* vm) {
    if (be_top(vm) != 3)
        be_raise(vm, "type_error", "set(index, rgb)");
    const auto h = handle(vm);
    if (h.generation)
        check(vm, strip::set(h,
                      static_cast<std::size_t>(integer(vm, 2, 0,
                          static_cast<int>(newmota::platform::led_strip::kMaximumPixels - 1))),
                      pixel(vm, 3)));
    be_return_nil(vm);
}

int Fill(bvm* vm) {
    if (be_top(vm) != 2)
        be_raise(vm, "type_error", "fill(rgb)");
    const auto h = handle(vm);
    if (h.generation)
        check(vm, strip::fill(h, pixel(vm, 2)));
    be_return_nil(vm);
}

int Show(bvm* vm) {
    const auto h = handle(vm);
    if (h.generation)
        check(vm, strip::show(h));
    be_return_nil(vm);
}

int Busy(bvm* vm) {
    const auto h = handle(vm);
    if (!h.generation)
        be_return_nil(vm);
    bool busy{};
    check(vm, strip::busy(h, busy));
    be_pushbool(vm, busy);
    be_return(vm);
}

int Close(bvm* vm) {
    const auto h = handle(vm);
    if (h.generation) {
        check(vm, strip::close(h));
        if (!strip::managed(h))
            handles[h.slot] = {};
    }
    be_pushnil(vm);
    be_setmember(vm, 1, ".generation");
    be_pop(vm, 1);
    be_return_nil(vm);
}

int Deinit(bvm* vm) {
    const auto h = handle(vm);
    if (h.generation)
        (void)strip::request_close(h);
    strip::poll();
    be_return_nil(vm);
}

const bnfuncinfo methods[] = {{".generation", nullptr}, {"init", Init}, {"set", Set},
    {"fill", Fill}, {"show", Show}, {"busy", Busy}, {"close", Close}, {"deinit", Deinit},
    {nullptr, nullptr}};
} // namespace

namespace newmota {
bool install_led_strip_module(bvm* vm) {
    be_pushclass(vm, "led_strip", methods);
    be_setglobal(vm, "led_strip");
    be_pop(vm, 1);
    return true;
}

void remove_led_strip_module() {
    for (auto& h : handles) {
        if (h.generation)
            (void)strip::request_close(h);
        h = {};
    }
    strip::poll();
}
} // namespace newmota
