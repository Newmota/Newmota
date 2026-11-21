/*
  mqtt_settings.h - bounded persistent MQTT settings for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "newmota_features.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace newmota {

/**
 * @brief Stores the bounded MQTT record shared by all compatible image roles.
 *
 * MQTT is disabled by default. The broker host may remain empty until MQTT is
 * enabled; an empty client identity selects the Tasmota-compatible default.
 * Runtime mutation and broker access methods are compiled only with USE_MQTT;
 * the persisted fields and validation remain available in every image.
 */
struct MqttSettings final {
    static constexpr std::size_t kHostCapacity = 128;
    static constexpr std::size_t kClientIdCapacity = 64;
    static constexpr std::size_t kUsernameCapacity = 64;
    static constexpr std::size_t kPasswordCapacity = 64;
    static constexpr std::size_t kTopicCapacity = 64;
    static constexpr std::size_t kFullTopicCapacity = 128;
    static constexpr std::size_t kPrefixCapacity = 32;
    static constexpr std::size_t kGroupCount = 4;
    static constexpr std::string_view kDefaultFullTopic = "%prefix%/%topic%/";
    static constexpr std::string_view kDefaultTopicTemplate = "Newmota_%06X";
    static constexpr std::string_view kDefaultClientIdTemplate = "DVES_%06X";

    std::array<char, kHostCapacity> host_storage{};
    std::array<char, kClientIdCapacity> client_id_storage{};
    std::array<char, kUsernameCapacity> username_storage{};
    std::array<char, kPasswordCapacity> password_storage{};
    std::array<char, kTopicCapacity> topic_storage{};
    std::array<char, kTopicCapacity> button_topic_storage{};
    std::uint8_t button_topic_length{};
    bool button_retain{};
    std::array<char, kTopicCapacity> switch_topic_storage{};
    std::uint8_t switch_topic_length{};
    bool switch_retain{};
    std::uint8_t host_length{};
    std::uint8_t client_id_length{};
    std::uint8_t username_length{};
    std::uint8_t password_length{};
    std::uint8_t topic_length{};
    std::array<char, kFullTopicCapacity> full_topic_storage{};
    std::array<std::array<char, kPrefixCapacity>, 3> prefix_storage{};
    std::array<std::array<char, kTopicCapacity>, kGroupCount> group_storage{};
    std::uint8_t full_topic_length{};
    std::array<std::uint8_t, 3> prefix_lengths{};
    std::array<std::uint8_t, kGroupCount> group_lengths{};
    std::uint16_t retry_seconds{10U};
    std::uint16_t timeout_seconds{15U};
    bool response_topic{};
    bool offline_lwt{true};
    bool group_topic_mode{};
    bool only_json{};
    bool no_retain{};
    bool power_retain{};
    bool info_retain{};
    bool state_retain{};
    bool status_retain{};
    bool power_index{};
    std::uint16_t port{1883U};
    std::uint16_t keep_alive_seconds{60U};
    bool enabled{};
    bool secure{};

    /** @brief Constructs disabled settings with the default base topic. */
    MqttSettings();

    /** @brief Restores disabled MQTT defaults without touching persistence. */
    void reset();
#if USE_MQTT
    /** @brief Copies a broker host or clears it with an empty value. */
    [[nodiscard]] bool set_host(std::string_view value);
    /** @brief Copies a client template or clears it to use the default. */
    [[nodiscard]] bool set_client_id(std::string_view value);
    /** @brief Copies an optional MQTT username. */
    [[nodiscard]] bool set_username(std::string_view value);
    /** @brief Copies an optional MQTT password. */
    [[nodiscard]] bool set_password(std::string_view value);
    /** @brief Copies a nonempty Tasmota base topic. */
    [[nodiscard]] bool set_topic(std::string_view value);
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    /** @brief Copies a ButtonTopic command topic, or clears it with an empty value. */
    [[nodiscard]] bool set_button_topic(std::string_view value);
    /** @brief Copies a SwitchTopic command topic, or clears it with an empty value. */
    [[nodiscard]] bool set_switch_topic(std::string_view value);
#endif
    [[nodiscard]] bool set_full_topic(std::string_view value);
    [[nodiscard]] bool set_prefix(std::size_t index, std::string_view value);
    [[nodiscard]] bool set_group_topic(std::size_t index, std::string_view value);
    /** @brief Sets a nonzero broker port. */
    [[nodiscard]] bool set_port(std::uint16_t value);

    /** @brief Returns the configured broker host. */
    [[nodiscard]] std::string_view host() const;
    /** @brief Returns the configured client identifier. */
    [[nodiscard]] std::string_view client_id() const;

    /** @brief Returns the configured client template or its Tasmota default. */
    [[nodiscard]] std::string_view client_id_template() const {
        const auto configured = client_id();
        return configured.empty() ? kDefaultClientIdTemplate : configured;
    }

    /** @brief Returns the configured username, if any. */
    [[nodiscard]] std::string_view username() const;
    /** @brief Returns the configured password, if any. */
    [[nodiscard]] std::string_view password() const;
#endif
    /** @brief Returns the configured Tasmota base topic. */
    [[nodiscard]] std::string_view topic() const;

    /** @brief Returns the retained application ButtonTopic, including in recovery images. */
    [[nodiscard]] std::string_view button_topic() const {
        return {button_topic_storage.data(), button_topic_length};
    }

    /** @brief Returns the retained application SwitchTopic, including in recovery images. */
    [[nodiscard]] std::string_view switch_topic() const {
        return {switch_topic_storage.data(), switch_topic_length};
    }

    [[nodiscard]] std::string_view full_topic() const {
        return {full_topic_storage.data(), full_topic_length};
    }

    [[nodiscard]] std::string_view prefix(std::size_t index) const {
        return index < 3U ? std::string_view{prefix_storage[index].data(), prefix_lengths[index]}
                          : std::string_view{};
    }

    [[nodiscard]] std::string_view group_topic(std::size_t index) const {
        return index < kGroupCount
                   ? std::string_view{group_storage[index].data(), group_lengths[index]}
                   : std::string_view{};
    }

    /** @brief Reports whether the record is internally safe and usable. */
    [[nodiscard]] bool valid() const;
#if USE_MQTT
    /** @brief Compares every persisted MQTT field. */
    [[nodiscard]] bool operator==(const MqttSettings& other) const;

    /** @brief Compares every persisted MQTT field. */
    [[nodiscard]] bool operator!=(const MqttSettings& other) const { return !(*this == other); }
#endif
};

} // namespace newmota
