/*
  commands.cpp - Tasmota-style Berry commands

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/berry/modules/newmota/commands.h"

#include "support/text.h"

extern "C" {
#include "be_gc.h"
#include "be_list.h"
#include "be_object.h"
#include "be_string.h"
#include "berry.h"
}

#include <algorithm>
#include <cstring>
#include <string_view>

namespace {

using newmota::BerryCommandCall;
using newmota::CommandOutput;

blist* command_roots{}; ///< Alternating registered name and function, in registration order.
BerryCommandCall* current_call{};

/** @brief Returns the number of registered commands. */
int command_count() {
    return command_roots == nullptr ? 0 : be_list_count(command_roots) / 2;
}

/** @brief Returns the registered name of command @p index. */
std::string_view command_name(const int index) {
    auto* name = var_tostr(be_list_at(command_roots, index * 2));
    return {str(name), static_cast<std::size_t>(str_len(name))};
}

/** @brief Appends to the current response; Tasmota's Response_P bounds it the same way. */
bool append_response(void* const context, const std::string_view text) {
    auto& call = *static_cast<BerryCommandCall*>(context);
    if (text.size() > newmota::kMaximumCommandResponseLength - call.response_size) {
        call.response_overflow = true;
        return false;
    }
    std::memcpy(call.response + call.response_size, text.data(), text.size());
    call.response_size += text.size();
    return true;
}

/** @brief Replaces the current response with `{"<Command>":<value>}`; @p value is written as
 * a JSON string when @p quoted, otherwise as given, such as `"Done"`.
 */
int respond(bvm* const vm, const std::string_view value, const bool quoted) {
    if (current_call == nullptr)
        be_return_nil(vm); // outside a command there is no response to set
    current_call->response_size = 0U;
    current_call->response_overflow = false;
    CommandOutput output{append_response, current_call};
    (void)(output.write("{") && output.json_string(current_call->name.data()) &&
           output.write(":") && (quoted ? output.json_string(value) : output.write(value)) &&
           output.write("}"));
    be_return_nil(vm);
}

} // namespace

namespace newmota {

int find_berry_command(const std::string_view name) {
    for (int index = 0; index < command_count(); ++index)
        if (support::ascii_iequals(command_name(index), name))
            return index;
    return -1;
}

BerryCommandCall* swap_berry_command_call(BerryCommandCall* const call) {
    auto* const previous = current_call;
    current_call = call;
    return previous;
}

int dispatch_berry_command(bvm* const vm) {
    auto* call = static_cast<BerryCommandCall*>(be_tocomptr(vm, 1));
    if (call == nullptr || call->command < 0 || call->command >= command_count())
        be_return_nil(vm);
    const auto name = command_name(call->command);
    const auto length = std::min(name.size(), call->name.size() - 1U);
    std::memcpy(call->name.data(), name.data(), length);
    call->name[length] = '\0';
    const auto& request = *call->request;

    be_pushnil(vm);
    var_setval(be_indexof(vm, -1), be_list_at(command_roots, call->command * 2 + 1));
    be_pushnstring(vm, name.data(), name.size());
    // Tasmota's index defaults to 1 without digits; the payload is "" without arguments.
    be_pushint(vm, request.has_index ? request.index : 1);
    be_pushnstring(vm, request.payload.data(), request.payload.size());
    const int base = be_top(vm);
    if (be_getmodule(vm, "json") && be_getmember(vm, -1, "load")) {
        be_remove(vm, -2);
        be_pushnstring(vm, request.payload.data(), request.payload.size());
        be_call(vm, 1); // nil when the payload is not JSON, as json.load() in Tasmota
        be_pop(vm, 1);
    } else {
        be_pop(vm, be_top(vm) - base);
        be_pushnil(vm);
    }
    be_call(vm, 4);
    be_return_nil(vm);
}

int add_berry_command(bvm* const vm) {
    // Tasmota's check_not_method(): a method would lose its instance; use a closure.
    const bvalue* function = be_top(vm) >= 2 ? be_indexof(vm, 2) : nullptr;
    if (function == nullptr || !be_isfunction(vm, 2))
        be_raise(vm, "type_error", "BRY: argument must be a function");
    if (var_isclosure(function) &&
        (static_cast<bclosure*>(var_toobj(function))->proto->varg & BE_VA_METHOD) != 0)
        be_raise(vm, "type_error",
            "BRY: method not allowed, use a closure like '/ args -> obj.func(args)'");
    if (!be_isstring(vm, 1))
        be_raise(vm, "type_error", "command name must be a string");
    const std::string_view name{be_tostring(vm, 1), static_cast<std::size_t>(be_strlen(vm, 1))};
    if (command_roots == nullptr) {
        auto* list = be_list_new(vm);
        be_pushnil(vm);
        var_setlist(be_indexof(vm, -1), list);
        be_gc_fix(vm, reinterpret_cast<bgcobject*>(list));
        be_pop(vm, 1);
        command_roots = list;
    }
    // Registering a name again replaces its function, as Tasmota's map does.
    for (int index = 0; index < command_count(); ++index) {
        if (command_name(index) == name) {
            var_setval(be_list_at(command_roots, index * 2 + 1), be_indexof(vm, 2));
            be_return_nil(vm);
        }
    }
    (void)be_list_push(vm, command_roots, be_indexof(vm, 1));
    (void)be_list_push(vm, command_roots, be_indexof(vm, 2));
    be_return_nil(vm);
}

int remove_berry_command(bvm* const vm) {
    if (be_top(vm) != 1 || !be_isstring(vm, 1))
        be_return_nil(vm);
    const std::string_view name{be_tostring(vm, 1), static_cast<std::size_t>(be_strlen(vm, 1))};
    for (int index = 0; index < command_count(); ++index) {
        if (command_name(index) == name) { // exact, as Tasmota's map.remove()
            (void)be_list_remove(vm, command_roots, index * 2 + 1);
            (void)be_list_remove(vm, command_roots, index * 2);
            break;
        }
    }
    be_return_nil(vm);
}

int berry_resp_cmnd(bvm* const vm) {
    if (be_top(vm) != 1)
        be_raise(vm, "type_error", nullptr);
    const char* const text = be_tostring(vm, 1);
    if (current_call == nullptr)
        be_return_nil(vm);
    current_call->response_size = 0U;
    current_call->response_overflow = false;
    CommandOutput output{append_response, current_call};
    (void)output.write(text);
    be_return_nil(vm);
}

int berry_resp_cmnd_str(bvm* const vm) {
    if (be_top(vm) != 1)
        be_raise(vm, "type_error", nullptr);
    const char* const text = be_tostring(vm, 1);
    return respond(vm, text, true);
}

int berry_resp_cmnd_done(bvm* const vm) {
    return respond(vm, "\"Done\"", false);
}

int berry_resp_cmnd_error(bvm* const vm) {
    return respond(vm, "\"Error\"", false);
}

int berry_resp_cmnd_failed(bvm* const vm) {
    return respond(vm, "\"Failed\"", false);
}

void remove_berry_commands(bvm* const vm) {
    if (command_roots != nullptr)
        be_gc_unfix(vm, reinterpret_cast<bgcobject*>(command_roots));
    command_roots = nullptr;
    current_call = nullptr;
}

} // namespace newmota
