/*
  widget_module.cpp - Berry widget module for the dashboard

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file widget_module.cpp
 * @brief `widget.set(id, html)` stores a widget's markup, `widget.data(id, value)` its data as
 * `json.dump(value)`; `widget.remove(id)` deletes it; `widget.on(id, fn)` receives the
 * widget's control actions; `widget.viewers()` reports whether a page is watching.
 *
 * The script is trusted to pass correct markup and data: nothing but the bounds that keep
 * memory and the 4 KiB response safe is checked. Untrusted text belongs in the data, which
 * the page only ever shows as text. Callbacks live in one fixed GC-rooted list indexed by the
 * registry slot of their widget.
 */
#include "services/web/berry/widget_module.h"
#include "services/web/widgets.h"
#include "newmota/platform/platform.h"

#include <cstdlib>
#include <string_view>
extern "C" {
#include "berry.h"
#include "be_gc.h"
#include "be_list.h"
#include "be_vm.h"
}

namespace {
namespace widgets = newmota::widgets;

blist* callbacks{};
bvm* callback_vm{};

std::string_view text(bvm* vm, int index) {
    return {be_tostring(vm, index), static_cast<std::size_t>(be_strlen(vm, index))};
}

void raise_bounds(bvm* vm, const widgets::SetResult result) {
    if (result == widgets::SetResult::unknown)
        be_raise(vm, "value_error", "widget id unknown, set its markup first");
    if (result != widgets::SetResult::ok)
        be_raise(vm, "value_error", "widget id, size or count out of bounds");
}

/** @brief `set(id, html [, order])` creates a widget or replaces its markup. */
int Set(bvm* vm) {
    if (!be_isstring(vm, 1) || !be_isstring(vm, 2))
        be_raise(vm, "type_error", "widget.set expects an id and an HTML string");
    // Without an order the widget keeps its place.
    const bool ordered = be_top(vm) >= 3 && be_isint(vm, 3);
    const auto order = ordered ? static_cast<std::int32_t>(be_toint(vm, 3)) : 0;
    raise_bounds(vm, widgets::set_markup(text(vm, 1), text(vm, 2), ordered ? &order : nullptr));
    be_return_nil(vm);
}

/** @brief `data(id, value)` replaces the data of an existing widget with `json.dump(value)`. */
int Data(bvm* vm) {
    if (!be_isstring(vm, 1) || be_top(vm) < 2)
        be_raise(vm, "type_error", "widget.data expects an id and a value");
    be_getmodule(vm, "json");
    be_getmember(vm, -1, "dump");
    be_pushvalue(vm, 2);
    be_call(vm, 1);
    be_pop(vm, 1);
    raise_bounds(vm, widgets::set_data(text(vm, 1), text(vm, -1)));
    be_return_nil(vm);
}

void clear_callback(int slot) {
    if (callbacks != nullptr && slot >= 0)
        var_setnil(be_list_at(callbacks, slot));
}

int Remove(bvm* vm) {
    if (!be_isstring(vm, 1))
        be_raise(vm, "type_error", "widget.remove expects an id");
    const auto id = text(vm, 1);
    clear_callback(widgets::slot(id));
    be_pushbool(vm, widgets::remove(id));
    be_return(vm);
}

/** @brief `on(id, fn)` replaces the widget's callback; `on(id, nil)` removes it. */
int On(bvm* vm) {
    if (be_top(vm) < 2 || !be_isstring(vm, 1) || !(be_isfunction(vm, 2) || be_isnil(vm, 2)))
        be_raise(vm, "type_error", "widget.on expects an id and a function or nil");
    const int slot = widgets::slot(text(vm, 1));
    if (slot < 0)
        be_raise(vm, "value_error", "widget id unknown, set its markup first");
    if (callbacks == nullptr) {
        // Keep the new list on the stack until it is fixed, so a collection cannot free it.
        auto* list = be_list_new(vm);
        be_pushnil(vm);
        var_setlist(be_indexof(vm, -1), list);
        be_list_resize(vm, list, static_cast<int>(widgets::kCapacity));
        be_gc_fix(vm, reinterpret_cast<bgcobject*>(list));
        be_pop(vm, 1);
        callbacks = list;
        callback_vm = vm;
    }
    var_setval(be_list_at(callbacks, slot), be_indexof(vm, 2));
    be_return_nil(vm);
}

int Viewers(bvm* vm) {
    be_pushbool(vm, widgets::viewers(newmota::platform::monotonic_milliseconds()));
    be_return(vm);
}

} // namespace

extern "C" {

be_native_module_attr_table(widget){
    be_native_module_function("set", Set),
    be_native_module_function("data", Data),
    be_native_module_function("remove", Remove),
    be_native_module_function("on", On),
    be_native_module_function("viewers", Viewers),
};

extern const bntvmodule_t be_native_module_widget = {
    "widget", widget_attrs, sizeof(widget_attrs) / sizeof(widget_attrs[0]), nullptr};

} // extern "C"

namespace newmota {

void remove_widget_module() {
    if (callbacks != nullptr && callback_vm != nullptr)
        be_gc_unfix(callback_vm, reinterpret_cast<bgcobject*>(callbacks));
    callbacks = nullptr;
    callback_vm = nullptr;
    widgets::clear();
}

bool widget_module_pending() {
    return widgets::actions_pending();
}

int dispatch_widget_action(bvm* vm) {
    // Actions are drained even without a callback, so the bounded queue never stays full.
    widgets::Action action{};
    if (!widgets::pop_action(action) || callbacks == nullptr)
        be_return_nil(vm);
    const int slot = widgets::slot(action.id.data());
    if (slot < 0)
        be_return_nil(vm);
    be_pushnil(vm);
    var_setval(be_indexof(vm, -1), be_list_at(callbacks, slot));
    if (!be_isfunction(vm, -1))
        be_return_nil(vm);
    be_pushstring(vm, action.name.data());
    be_pushstring(vm, action.value.data());
    be_call(vm, 2);
    be_return_nil(vm);
}

} // namespace newmota
