/*
  mqtt_module.cpp - Berry MQTT bridge and module bindings

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file mqtt_module.cpp
 * @brief Berry MQTT bridge and module bindings.
 */

#include "newmota_features.h"
#if USE_MQTT && USE_BERRY
#include "newmota/log.h"
#include "services/mqtt/berry/mqtt_module.h"
#include "services/mqtt/mqtt_client.h"
#include "services/mqtt/mqtt_service.h"
extern "C" {
#include "be_gc.h"
#include "be_list.h"
#include "be_string.h"
#include "berry.h"
}
#include <algorithm>
#include <array>
#include <cstring>

namespace {
newmota::BerryMqttBridge* bridge{};

std::string_view filter_at(const blist* list, const int index) {
    const auto* value = be_list_at(list, index);
    const auto* text = var_tostr(value);
    return {str(text), static_cast<std::size_t>(str_len(text))};
}

void push_list(bvm* vm, blist* list) {
    be_pushnil(vm);
    var_setlist(be_indexof(vm, -1), list);
}

void push_value(bvm* vm, const bvalue& value) {
    be_pushnil(vm);
    var_setval(be_indexof(vm, -1), &value);
}

int Subscribe(bvm* vm) {
    if (!bridge)
        be_raise(vm, "runtime_error", "MQTT binding unavailable");
    return bridge->subscribe(vm);
}

int Unsubscribe(bvm* vm) {
    if (!bridge)
        be_raise(vm, "runtime_error", "MQTT binding unavailable");
    return bridge->unsubscribe(vm);
}

int Publish(bvm* vm) {
    if (!bridge)
        be_raise(vm, "runtime_error", "MQTT binding unavailable");
    return bridge->publish(vm);
}

int Connected(bvm* vm) {
    be_pushbool(vm, bridge && bridge->connected());
    be_return(vm);
}
} // namespace

extern "C" {
be_native_module_attr_table(mqtt){
    be_native_module_function("publish", Publish),
    be_native_module_function("subscribe", Subscribe),
    be_native_module_function("unsubscribe", Unsubscribe),
    be_native_module_function("connected", Connected),
};
extern const bntvmodule_t be_native_module_mqtt = {
    "mqtt", mqtt_attrs, sizeof(mqtt_attrs) / sizeof(mqtt_attrs[0]), nullptr};
}

namespace newmota {
void install_mqtt_module(bvm* vm, BerryMqttBridge& owner) {
    bridge = &owner;
    owner.install(vm);
}

void remove_mqtt_module() {
    if (bridge)
        bridge->remove();
    bridge = nullptr;
}

void BerryMqttBridge::install(bvm* vm) {
    vm_ = vm;
}

void BerryMqttBridge::remove() {
    if (listeners_)
        be_gc_unfix(vm_, reinterpret_cast<bgcobject*>(listeners_));
    listeners_ = nullptr;
    vm_ = nullptr;
    if (client_)
        (void)client_->set_script_subscriptions(nullptr, 0);
}

void BerryMqttBridge::bind(MqttClient* client, MqttService* service) {
    client_ = client;
    service_ = service;
    synchronize();
}

bool BerryMqttBridge::connected() const {
    return client_ && service_ && service_->enabled() && !client_->connection_test_active() &&
           client_->state() == MqttTransportState::connected;
}

void BerryMqttBridge::synchronize() {
    if (!client_)
        return;
    std::array<std::string_view, MqttClient::kScriptFilterLimit> filters{};
    std::size_t count = 0;
    if (listeners_)
        for (int i = 0; i < listeners_->count; i += 2) {
            const auto filter = filter_at(listeners_, i);
            bool found = false;
            for (std::size_t j = 0; j < count; ++j)
                if (filters[j] == filter)
                    found = true;
            if (!found)
                filters[count++] = filter;
        }
    (void)client_->set_script_subscriptions(filters.data(), count);
}

void BerryMqttBridge::replace_listeners(blist* next) {
    be_gc_fix(vm_, reinterpret_cast<bgcobject*>(next));
    if (listeners_)
        be_gc_unfix(vm_, reinterpret_cast<bgcobject*>(listeners_));
    listeners_ = next;
    synchronize();
}

bool BerryMqttBridge::matches(std::string_view filter, std::string_view topic) {
    // MQTT reserves leading '$' topics from root wildcard filters.
    if (!topic.empty() && topic.front() == '$' && !filter.empty() &&
        (filter.front() == '+' || filter.front() == '#'))
        return false;
    while (true) {
        const auto fslash = filter.find('/'), tslash = topic.find('/');
        const auto f = filter.substr(0, fslash), t = topic.substr(0, tslash);
        if (f == "#")
            return true;
        if (f != "+" && f != t)
            return false;
        if (fslash == std::string_view::npos)
            return tslash == std::string_view::npos;
        filter.remove_prefix(fslash + 1);
        if (tslash == std::string_view::npos)
            return filter == "#";
        topic.remove_prefix(tslash + 1);
    }
}

int BerryMqttBridge::subscribe(bvm* vm) {
    const int argc = be_top(vm);
    if (argc < 1 || argc > 2 || !be_isstring(vm, 1))
        be_raise(vm, "type_error", "mqtt.subscribe expects a filter and optional callback");
    const std::string_view filter{be_tostring(vm, 1), static_cast<std::size_t>(be_strlen(vm, 1))};
    if (!MqttClient::valid_filter(filter))
        be_raise(vm, "value_error", "invalid MQTT filter");
    if (argc == 1 || !be_isfunction(vm, 2)) {
        be_pushnil(vm);
        if (argc == 2)
            be_moveto(vm, -1, 2);
    }
    const int callback = 2;
    std::array<std::string_view, MqttClient::kScriptFilterLimit> filters{};
    std::size_t count = 0;
    bool existing_filter = false;
    if (listeners_)
        for (int i = 0; i < listeners_->count; i += 2) {
            const auto registered = filter_at(listeners_, i);
            bool found = false;
            for (std::size_t j = 0; j < count; ++j)
                if (filters[j] == registered)
                    found = true;
            if (!found)
                filters[count++] = registered;
            if (registered == filter) {
                existing_filter = true;
                push_value(vm, *be_list_at(listeners_, i + 1));
                be_pushvalue(vm, callback);
                const bool same = be_iseq(vm);
                be_pop(vm, 2);
                if (same)
                    be_return_nil(vm);
            }
        }
    if ((!existing_filter && count == filters.size()) ||
        (listeners_ && listeners_->count >= static_cast<int>(kListenerLimit * 2)))
        be_raise(vm, "value_error", "MQTT listener capacity exceeded");
    // Build a replacement first: allocation failure cannot partially register a pair.
    auto* next = listeners_ ? be_list_copy(vm, listeners_) : be_list_new(vm);
    push_list(vm, next);
    (void)be_list_push(vm, next, be_indexof(vm, 1));
    (void)be_list_push(vm, next, be_indexof(vm, callback));
    replace_listeners(next);
    be_return_nil(vm);
}

int BerryMqttBridge::unsubscribe(bvm* vm) {
    if (be_top(vm) > 1 || (be_top(vm) == 1 && !be_isnil(vm, 1) && !be_isstring(vm, 1)))
        be_raise(vm, "type_error", "mqtt.unsubscribe expects an optional filter");
    if (!listeners_)
        be_return_nil(vm);
    const bool all = be_top(vm) == 0 || be_isnil(vm, 1);
    const std::string_view filter =
        all ? std::string_view{}
            : std::string_view{be_tostring(vm, 1), static_cast<std::size_t>(be_strlen(vm, 1))};
    auto* next = be_list_new(vm);
    push_list(vm, next);
    for (int i = 0; i < listeners_->count; i += 2)
        if (!all && filter_at(listeners_, i) != filter) {
            (void)be_list_push(vm, next, be_list_at(listeners_, i));
            (void)be_list_push(vm, next, be_list_at(listeners_, i + 1));
        }
    replace_listeners(next);
    be_return_nil(vm);
}

int BerryMqttBridge::publish(bvm* vm) {
    const int argc = be_top(vm);
    if (argc < 2 || argc > 6 || !be_isstring(vm, 1) || (!be_isstring(vm, 2) && !be_isbytes(vm, 2)))
        be_raise(vm, "type_error", "mqtt.publish expects topic and string or bytes payload");
    const bool binary = be_isbytes(vm, 2);
    if (!binary && argc >= 4 && !be_isnil(vm, 4))
        be_raise(vm, "argument_error", "start and len are not allowed with string payloads");
    const bint requested_start = argc >= 4 && !be_isnil(vm, 4) ? be_toint(vm, 4) : 0;
    const bint requested_length = argc >= 5 && !be_isnil(vm, 5) ? be_toint(vm, 5) : -1;
    const bool retained = argc >= 3 && !be_isnil(vm, 3) && be_tobool(vm, 3);
    const bint level = argc >= 6 && !be_isnil(vm, 6) ? be_toint(vm, 6) : 2;
    // Optional conversions may execute Berry methods. Read the bytes pointer
    // afterwards so a conversion that resizes the payload cannot invalidate it.
    const auto topic =
        std::string_view{be_tostring(vm, 1), static_cast<std::size_t>(be_strlen(vm, 1))};
    std::size_t size = 0;
    const char* data =
        binary ? static_cast<const char*>(be_tobytes(vm, 2, &size)) : be_tostring(vm, 2);
    if (!binary)
        size = std::strlen(data);
    if (binary) {
        const auto start =
            std::min(size, static_cast<std::size_t>(std::max<bint>(0, requested_start)));
        if (data)
            data += start;
        size -= start;
        if (requested_length >= 0)
            size = std::min(size, static_cast<std::size_t>(requested_length));
    }
    if (connected()) {
        if (!service_->queue_publish(topic, {data ? data : "", size}, retained))
            be_raise(vm, "value_error", "MQTT publication rejected or queue full");
        if (level > 0 && level <= 4)
            AddLog(static_cast<LogLevel>(level), "MQT: Berry publish %.*s (%u bytes)",
                static_cast<int>(topic.size()), topic.data(), static_cast<unsigned int>(size));
    }
    be_return_nil(vm);
}

int BerryMqttBridge::dispatch(bvm* vm, const MqttPublishPacket& packet) {
    if (!listeners_) {
        be_pushbool(vm, false);
        be_return(vm);
    }
    // Snapshot callback values before invoking user code; mutations apply to the next
    // delivery.
    be_newlist(vm);
    const int snapshot = be_absindex(vm, -1);
    for (int i = 0; i < listeners_->count; i += 2) {
        const auto* callback = be_list_at(listeners_, i + 1);
        if (!var_isnil(callback) && matches(filter_at(listeners_, i), packet.topic)) {
            push_value(vm, *callback);
            be_data_push(vm, snapshot);
            be_pop(vm, 1);
        }
    }
    const int count = be_data_size(vm, snapshot);
    bool handled = false;
    for (int i = 0; i < count; ++i) {
        be_pushint(vm, i);
        (void)be_getindex(vm, snapshot);
        be_remove(vm, -2);
        const int callback = be_absindex(vm, -1);
        be_pushnstring(vm, packet.topic.data(), packet.topic.size());
        be_pushint(vm, 0);
        const auto* payload = reinterpret_cast<const char*>(packet.payload);
        const auto* nul = packet.payload_size ? static_cast<const char*>(
                                                    std::memchr(payload, 0, packet.payload_size))
                                              : nullptr;
        const auto text_size = nul ? static_cast<std::size_t>(nul - payload) : packet.payload_size;
        be_pushnstring(vm, payload ? payload : "", text_size);
        if (packet.payload_size)
            be_pushbytes(vm, packet.payload, packet.payload_size);
        else
            be_pushnil(vm);
        be_call(vm, 4);
        handled = be_isnil(vm, callback) || be_tobool(vm, callback) || handled;
        be_pop(vm, 5);
    }
    be_pushbool(vm, handled);
    be_return(vm);
}

int dispatch_mqtt_callbacks(bvm* vm) {
    const auto* packet = static_cast<const MqttPublishPacket*>(be_tocomptr(vm, 1));
    if (!bridge) {
        be_pushbool(vm, false);
        be_return(vm);
    }
    return bridge->dispatch(vm, *packet);
}
} // namespace newmota
#endif
