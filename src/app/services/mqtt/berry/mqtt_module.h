/*
  mqtt_module.h - Berry MQTT bridge interface

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#pragma once

#include "newmota_features.h"
#include <cstddef>
#include <string_view>
struct bvm;
struct blist;

namespace newmota {
class MqttClient;
class MqttService;
struct MqttPublishPacket;

/** @brief Resident Berry integration; runtime pointers are nullable and non-owning. */
class BerryMqttBridge final {
  public:
    static constexpr std::size_t kListenerLimit = USE_MATTER_BRIDGE ? 32 : 16;
    /** @brief Attaches the app-owned MQTT client and publication policy. */
    void bind(MqttClient* client, MqttService* service);
    /** @brief Installs Berry callbacks and the MQTT event listener. */
    void install(bvm* vm);
    /** @brief Removes listeners and clears non-owning service pointers. */
    void remove();
    /** @brief Reports whether the bound MQTT client is connected. */
    [[nodiscard]] bool connected() const;
    /** @brief Tests a topic against an MQTT subscription filter. */
    [[nodiscard]] static bool matches(std::string_view filter, std::string_view topic);
    /** @brief Adds a Berry listener for the supplied MQTT topic filter. */
    int subscribe(bvm* vm);
    /** @brief Removes a Berry listener for the supplied MQTT topic filter. */
    int unsubscribe(bvm* vm);
    /** @brief Publishes a Berry-provided topic and payload through MQTT policy. */
    int publish(bvm* vm);
    /** @brief Runs inside BerryService's protected callback and execution deadline. */
    int dispatch(bvm* vm, const MqttPublishPacket& packet);

  private:
    void synchronize();
    void replace_listeners(blist* listeners);
    bvm* vm_{};
    blist* listeners_{}; // fixed GC root; children are traced by the VM
    MqttClient* client_{};
    MqttService* service_{};
};

/** @brief Installs MQTT Berry bindings on the supplied VM. */
void install_mqtt_module(bvm* vm, BerryMqttBridge& bridge);
/** @brief Removes MQTT Berry bindings before VM destruction. */
void remove_mqtt_module();
/** @brief Native protected entry receiving a packet pointer as its sole argument. */
int dispatch_mqtt_callbacks(bvm* vm);
} // namespace newmota
