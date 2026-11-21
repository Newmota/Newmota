/*
  mqtt_settings.cpp - bounded persistent MQTT settings for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.
*/

#include "services/mqtt/mqtt_settings.h"

#include <cstring>

namespace newmota {
namespace {

#if USE_MQTT
/** @brief Copies bounded text and rejects embedded NUL bytes. */
template <std::size_t N>
bool copy_text(std::array<char, N>& destination, std::uint8_t& length, const std::string_view value,
    const bool allow_empty) {
    if ((!allow_empty && value.empty()) || value.size() >= N ||
        value.find('\0') != std::string_view::npos) {
        return false;
    }
    destination.fill('\0');
    if (!value.empty()) {
        std::memcpy(destination.data(), value.data(), value.size());
    }
    length = static_cast<std::uint8_t>(value.size());
    return true;
}
#endif

/** @brief Validates one length-prefixed text field in a persisted record. */
template <std::size_t N>
bool valid_text(
    const std::array<char, N>& storage, const std::uint8_t length, const bool allow_empty) {
    if ((!allow_empty && length == 0U) || length >= N)
        return false;
    return std::string_view{storage.data(), length}.find('\0') == std::string_view::npos;
}

/** @brief Reports whether a base topic contains forbidden MQTT wildcards. */
bool valid_topic(
    const std::array<char, MqttSettings::kTopicCapacity>& storage, const std::uint8_t length) {
    if (!valid_text(storage, length, false))
        return false;
    const std::string_view value{storage.data(), length};
    if (value.front() == '/' || value.back() == '/' || value.find('+') != std::string_view::npos ||
        value.find('#') != std::string_view::npos ||
        value.find_first_of("\r\n") != std::string_view::npos) {
        return false;
    }

    // The runtime expands %06X to six hexadecimal characters. Reject a
    // template whose expansion would exceed the service's 63-byte base-topic
    // limit, so recovery AP startup cannot be left without an identity.
    std::size_t expanded_size = 0U;
    for (std::size_t index = 0U; index < value.size();) {
        const auto expansion = value[index] == '%' && value.substr(index, 4U) == "%06X" ? 6U : 1U;
        if (expanded_size + expansion >= MqttSettings::kTopicCapacity) {
            return false;
        }
        expanded_size += expansion;
        index += expansion == 6U ? 4U : 1U;
    }
    return true;
}

} // namespace

/** @brief Constructs disabled MQTT settings with the default base topic. */
MqttSettings::MqttSettings() {
    reset();
}

/** @brief Restores disabled MQTT defaults without touching persistence. */
void MqttSettings::reset() {
    host_storage.fill('\0');
    client_id_storage.fill('\0');
    username_storage.fill('\0');
    password_storage.fill('\0');
    topic_storage.fill('\0');
    button_topic_storage.fill('\0');
    button_topic_length = 0U;
    button_retain = false;
    switch_topic_storage.fill('\0');
    switch_topic_length = 0U;
    switch_retain = false;
    host_length = 0U;
    client_id_length = 0U;
    username_length = 0U;
    password_length = 0U;
    topic_length = 0U;
    port = 1883U;
    keep_alive_seconds = 60U;
    full_topic_storage.fill(0);
    std::memcpy(full_topic_storage.data(), kDefaultFullTopic.data(), kDefaultFullTopic.size());
    full_topic_length = kDefaultFullTopic.size();
    constexpr std::string_view prefixes[]{"cmnd", "stat", "tele"};
    for (std::size_t i = 0; i < 3U; ++i) {
        prefix_storage[i].fill(0);
        std::memcpy(prefix_storage[i].data(), prefixes[i].data(), prefixes[i].size());
        prefix_lengths[i] = static_cast<std::uint8_t>(prefixes[i].size());
    }
    for (auto& group : group_storage)
        group.fill(0);
    group_lengths.fill(0);
    std::memcpy(group_storage[0].data(), "tasmotas", 8U);
    group_lengths[0] = 8U;
    retry_seconds = 10U;
    timeout_seconds = 15U;
    response_topic = group_topic_mode = only_json = no_retain = false;
    power_retain = info_retain = state_retain = status_retain = power_index = false;
    offline_lwt = true;
    enabled = false;
    secure = false;
    std::memcpy(topic_storage.data(), kDefaultTopicTemplate.data(), kDefaultTopicTemplate.size());
    topic_length = static_cast<std::uint8_t>(kDefaultTopicTemplate.size());
}

#if USE_MQTT
/** @brief Copies a broker host or clears it with an empty value. */
bool MqttSettings::set_host(const std::string_view value) {
    return copy_text(host_storage, host_length, value, true);
}

/** @brief Copies a client identifier or clears it while disabled. */
bool MqttSettings::set_client_id(const std::string_view value) {
    return copy_text(client_id_storage, client_id_length, value, true);
}

/** @brief Copies an optional MQTT username. */
bool MqttSettings::set_username(const std::string_view value) {
    return copy_text(username_storage, username_length, value, true);
}

/** @brief Copies an optional MQTT password. */
bool MqttSettings::set_password(const std::string_view value) {
    return copy_text(password_storage, password_length, value, true);
}

/** @brief Copies a nonempty Tasmota base topic. */
bool MqttSettings::set_topic(const std::string_view value) {
    std::array<char, kTopicCapacity> candidate{};
    std::uint8_t candidate_length = 0U;
    if (!copy_text(candidate, candidate_length, value, false) ||
        !valid_topic(candidate, candidate_length)) {
        return false;
    }
    topic_storage = candidate;
    topic_length = candidate_length;
    return true;
}

#ifndef NEWMOTA_IMAGE_SAFEBOOT
bool MqttSettings::set_button_topic(const std::string_view value) {
    std::array<char, kTopicCapacity> candidate{};
    std::uint8_t length = 0U;
    if (!copy_text(candidate, length, value, true) ||
        (!value.empty() && !valid_topic(candidate, length)))
        return false;
    button_topic_storage = candidate;
    button_topic_length = length;
    return true;
}

bool MqttSettings::set_switch_topic(const std::string_view value) {
    std::array<char, kTopicCapacity> candidate{};
    std::uint8_t length = 0U;
    if (!copy_text(candidate, length, value, true) ||
        (!value.empty() && !valid_topic(candidate, length)))
        return false;
    switch_topic_storage = candidate;
    switch_topic_length = length;
    return true;
}
#endif

bool MqttSettings::set_full_topic(const std::string_view value) {
    if (value.find_first_of("+#\r\n") != std::string_view::npos)
        return false;
    return copy_text(full_topic_storage, full_topic_length, value, false);
}

bool MqttSettings::set_prefix(const std::size_t index, const std::string_view value) {
    if (index >= 3U || value.find_first_of("/+#%\r\n") != std::string_view::npos)
        return false;
    return copy_text(prefix_storage[index], prefix_lengths[index], value, false);
}

bool MqttSettings::set_group_topic(const std::size_t index, const std::string_view value) {
    if (index >= kGroupCount || value.find_first_of("+#\r\n") != std::string_view::npos ||
        (!value.empty() && (value.front() == '/' || value.back() == '/')))
        return false;
    return copy_text(group_storage[index], group_lengths[index], value, true);
}

/** @brief Sets a nonzero broker port. */
bool MqttSettings::set_port(const std::uint16_t value) {
    if (value == 0U)
        return false;
    port = value;
    return true;
}

/** @brief Returns the configured broker host. */
std::string_view MqttSettings::host() const {
    return {host_storage.data(), host_length};
}

/** @brief Returns the configured client identifier. */
std::string_view MqttSettings::client_id() const {
    return {client_id_storage.data(), client_id_length};
}

/** @brief Returns the configured username, if any. */
std::string_view MqttSettings::username() const {
    return {username_storage.data(), username_length};
}

/** @brief Returns the configured password, if any. */
std::string_view MqttSettings::password() const {
    return {password_storage.data(), password_length};
}
#endif

/** @brief Returns the configured Tasmota base topic. */
std::string_view MqttSettings::topic() const {
    return {topic_storage.data(), topic_length};
}

/** @brief Reports whether the record is internally safe and usable. */
bool MqttSettings::valid() const {
    if (!valid_text(button_topic_storage, button_topic_length, true) ||
        (button_topic_length != 0U && !valid_topic(button_topic_storage, button_topic_length)))
        return false;
    if (!valid_text(switch_topic_storage, switch_topic_length, true) ||
        (switch_topic_length != 0U && !valid_topic(switch_topic_storage, switch_topic_length)))
        return false;
    if (!valid_text(host_storage, host_length, true) ||
        !valid_text(client_id_storage, client_id_length, true) ||
        !valid_text(username_storage, username_length, true) ||
        !valid_text(password_storage, password_length, true) ||
        !valid_topic(topic_storage, topic_length) || port == 0U ||
        (password_length != 0U && username_length == 0U)) {
        return false;
    }
    if (!valid_text(full_topic_storage, full_topic_length, false) ||
        full_topic().find_first_of("+#\r\n") != std::string_view::npos || retry_seconds < 10U ||
        retry_seconds > 32000U || timeout_seconds == 0U || timeout_seconds > 100U ||
        (power_retain && only_json))
        return false;
    std::size_t topic_expansion = 0U;
    const auto format = full_topic();
    for (std::size_t i = 0U; i < format.size();) {
        if (format[i] != '%') {
            ++topic_expansion;
            ++i;
            continue;
        }
        const auto end = format.find('%', i + 1U);
        if (end == std::string_view::npos)
            return false;
        const auto token = format.substr(i, end - i + 1U);
        if (token == "%prefix%") {
            std::size_t maximum = 0U;
            for (const auto n : prefix_lengths)
                if (n > maximum)
                    maximum = n;
            topic_expansion += maximum;
        } else if (token == "%topic%")
            topic_expansion += kTopicCapacity - 1U;
        else if (token == "%hostname%")
            topic_expansion += 31U;
        else if (token == "%id%")
            topic_expansion += 12U;
        else
            return false;
        i = end + 1U;
    }
    if (format.find("%prefix%") == std::string_view::npos)
        topic_expansion += 1U + prefix_lengths[0];
    if (topic_expansion + 24U >= 256U)
        return false;
    for (std::size_t i = 0; i < 3U; ++i) {
        if (!valid_text(prefix_storage[i], prefix_lengths[i], false) ||
            prefix(i).find_first_of("/+#%\r\n") != std::string_view::npos)
            return false;
    }
    for (std::size_t i = 0; i < kGroupCount; ++i) {
        if (!valid_text(group_storage[i], group_lengths[i], true) ||
            group_topic(i).find_first_of("+#\r\n") != std::string_view::npos ||
            (!group_topic(i).empty() &&
                (group_topic(i).front() == '/' || group_topic(i).back() == '/')) ||
            group_topic(i) == topic())
            return false;
    }
    return !enabled || host_length != 0U;
}

#if USE_MQTT
/** @brief Compares every persisted MQTT field. */
bool MqttSettings::operator==(const MqttSettings& other) const {
    return host_length == other.host_length && client_id_length == other.client_id_length &&
           username_length == other.username_length && password_length == other.password_length &&
           topic_length == other.topic_length && port == other.port &&
           keep_alive_seconds == other.keep_alive_seconds && enabled == other.enabled &&
           secure == other.secure && host_storage == other.host_storage &&
           client_id_storage == other.client_id_storage &&
           username_storage == other.username_storage &&
           password_storage == other.password_storage && topic_storage == other.topic_storage &&
           full_topic_storage == other.full_topic_storage &&
           full_topic_length == other.full_topic_length && prefix_storage == other.prefix_storage &&
           prefix_lengths == other.prefix_lengths && group_storage == other.group_storage &&
           group_lengths == other.group_lengths && retry_seconds == other.retry_seconds &&
           timeout_seconds == other.timeout_seconds && response_topic == other.response_topic &&
           offline_lwt == other.offline_lwt && group_topic_mode == other.group_topic_mode &&
           only_json == other.only_json && no_retain == other.no_retain &&
           power_retain == other.power_retain && info_retain == other.info_retain &&
           state_retain == other.state_retain && status_retain == other.status_retain &&
           power_index == other.power_index && button_topic_storage == other.button_topic_storage &&
           button_topic_length == other.button_topic_length &&
           button_retain == other.button_retain &&
           switch_topic_storage == other.switch_topic_storage &&
           switch_topic_length == other.switch_topic_length && switch_retain == other.switch_retain;
}
#endif

} // namespace newmota
