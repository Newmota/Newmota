/*
  mqtt_service.cpp - bounded MQTT publication policy for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file mqtt_service.cpp
 * @brief Bounded MQTT publication and command-routing policy.
 */

#include "services/mqtt/mqtt_service.h"
#include "services/settings/state_text.h"
#include "services/status/status_commands.h"
#include "services/mqtt/mqtt_wire.h"

#include <cstring>
#include <cstdio>
#include <cctype>
#ifndef NEWMOTA_IMAGE_SAFEBOOT
#include "newmota/log.h"
#include "newmota/platform/network.h"
#include "services/network/network_identity.h"
#endif

namespace newmota {
namespace {

/** @brief Returns whether an MQTT base topic contains a forbidden wildcard. */
bool has_wildcard(const std::string_view topic) {
    return topic.find('+') != std::string_view::npos || topic.find('#') != std::string_view::npos ||
           topic.find('\0') != std::string_view::npos;
}

/** @brief Builds one bounded topic from a prefix, base, and suffix. */
template <std::size_t Capacity>
bool build_topic(std::array<char, Capacity>& destination, std::size_t& size,
    const std::string_view prefix, const std::string_view base, const std::string_view suffix) {
    const auto required = prefix.size() + base.size() + suffix.size();
    if (required >= destination.size())
        return false;
    std::size_t position = 0U;
    std::memcpy(destination.data() + position, prefix.data(), prefix.size());
    position += prefix.size();
    std::memcpy(destination.data() + position, base.data(), base.size());
    position += base.size();
    std::memcpy(destination.data() + position, suffix.data(), suffix.size());
    position += suffix.size();
    destination[position] = '\0';
    size = position;
    return true;
}

bool resolve_path(const MqttSettings& settings, const std::size_t prefix,
    const std::string_view topic, const std::string_view hostname, const std::string_view id,
    const std::string_view suffix, std::array<char, MqttService::kTopicCapacity>& output) {
    output.fill(0);
    std::size_t size = 0U;
    auto append = [&](const std::string_view value) {
        for (const auto c : value) {
            if (c == '/' && (size == 0U || output[size - 1U] == '/'))
                continue;
            if (c == '+' || c == '#' || c == '\0' || c == '\r' || c == '\n' ||
                size + 1U >= output.size())
                return false;
            output[size++] = c;
        }
        return true;
    };
    const auto format = settings.full_topic();
    for (std::size_t i = 0U; i < format.size();) {
        if (format[i] != '%') {
            if (!append(format.substr(i++, 1U)))
                return false;
            continue;
        }
        const auto end = format.find('%', i + 1U);
        if (end == std::string_view::npos)
            return false;
        const auto token = format.substr(i, end - i + 1U);
        const auto value = token == "%prefix%"     ? settings.prefix(prefix)
                           : token == "%topic%"    ? topic
                           : token == "%hostname%" ? hostname
                           : token == "%id%"       ? id
                                                   : std::string_view{};
        if (token != "%prefix%" && token != "%topic%" && token != "%hostname%" && token != "%id%")
            return false;
        if (!append(value))
            return false;
        i = end + 1U;
    }
    if (prefix == 0U && format.find("%prefix%") == std::string_view::npos) {
        if (!append("/") || !append(settings.prefix(0)))
            return false;
    }
    if (!append("/") || !append(suffix))
        return false;
    return size != 0U;
}

std::uint32_t publication_hash(const std::string_view topic, const std::string_view payload) {
    std::uint32_t hash = 2166136261U;
    for (const char c : topic)
        hash = (hash ^ static_cast<unsigned char>(c)) * 16777619U;
    hash = (hash ^ 0xffU) * 16777619U;
    for (const char c : payload)
        hash = (hash ^ static_cast<unsigned char>(c)) * 16777619U;
    return hash == 0U ? 1U : hash;
}

template <std::size_t N>
bool append_json(std::array<char, N>& output, std::size_t& size, const std::string_view text,
    const bool quote = false) {
    const auto put = [&](char c) {
        if (size + 1U >= N)
            return false;
        output[size++] = c;
        return true;
    };
    if (quote && !put('"'))
        return false;
    for (const auto c : text) {
        if (quote && (c == '"' || c == '\\')) {
            if (!put('\\'))
                return false;
        }
        if (static_cast<unsigned char>(c) < 32U)
            return false;
        if (!put(c))
            return false;
    }
    if (quote && !put('"'))
        return false;
    return true;
}

bool append_suffix(const std::string_view root, const std::string_view suffix,
    std::array<char, MqttService::kTopicCapacity>& output) {
    if (root.size() + suffix.size() >= output.size())
        return false;
    output.fill(0);
    std::memcpy(output.data(), root.data(), root.size());
    std::memcpy(output.data() + root.size(), suffix.data(), suffix.size());
    return true;
}
} // namespace

/** @brief Binds a canonical message cache and starts with the default topic. */
MqttService::MqttService(MessageRouter& messages, const MqttTransport transport)
    : messages_(messages), transport_(transport) {
    response_consumer_ = messages_.subscribe();
    (void)set_topic("Newmota");
}

bool MqttService::preview_topics(const MqttSettings& settings,
    const std::string_view resolved_topic, const std::string_view resolved_client,
    MqttTopicPreview& preview, const std::string_view hostname, const std::string_view id) {
    if (!settings.valid() || resolved_topic.empty() ||
        resolved_client.size() >= MqttSettings::kClientIdCapacity)
        return false;
    preview = {};
    if (!resolve_path(settings, 0U, resolved_topic, hostname, id, "", preview.command) ||
        !resolve_path(settings, 1U, resolved_topic, hostname, id, "RESULT", preview.result) ||
        !resolve_path(settings, 2U, resolved_topic, hostname, id, "STATE", preview.state) ||
        !resolve_path(settings, 2U, resolved_topic, hostname, id, "LWT", preview.lwt))
        return false;
    if (!resolved_client.empty()) {
        if (resolved_client.find_first_of("+#/\r\n") != std::string_view::npos)
            return false;
        const auto count = std::snprintf(preview.fallback.data(), preview.fallback.size(),
            "cmnd/%.*s_fb/", static_cast<int>(resolved_client.size()), resolved_client.data());
        if (count < 0 || static_cast<std::size_t>(count) >= preview.fallback.size())
            return false;
    }
    if (settings.full_topic().find("%topic%") != std::string_view::npos) {
        for (std::size_t i = 0U; i < MqttSettings::kGroupCount; ++i) {
            const auto group = settings.group_topic(i);
            if (group.empty())
                continue;
            if (group == resolved_topic)
                return false;
            if (settings.group_topic_mode) {
                if (!append_suffix("cmnd/", group, preview.groups[i]))
                    return false;
                const auto n = std::strlen(preview.groups[i].data());
                if (n + 1U >= preview.groups[i].size())
                    return false;
                preview.groups[i][n] = '/';
            } else if (!resolve_path(settings, 0U, group, hostname, id, "", preview.groups[i]))
                return false;
        }
    } else
        preview.fallback.fill(0);
    return true;
}

bool MqttService::configure(const MqttSettings& settings, const std::string_view resolved_topic,
    const std::string_view resolved_client, const std::string_view hostname,
    const std::string_view id) {
    MqttTopicPreview preview;
    if (!preview_topics(settings, resolved_topic, resolved_client, preview, hostname, id) ||
        !set_topic(resolved_topic))
        return false;
    config_ = settings;
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    discard_session_publications();
    key_clears_ = {};
    button_identity_.fill(0);
    if (id.size() < button_identity_.size())
        std::memcpy(button_identity_.data(), id.data(), id.size());
#endif
    preview_ = preview;
    parity_configured_ = true;
    std::memcpy(resolved_client_.data(), resolved_client.data(), resolved_client.size());
    resolved_client_[resolved_client.size()] = 0;
    hostname_.fill(0);
    if (hostname.size() < hostname_.size())
        std::memcpy(hostname_.data(), hostname.data(), hostname.size());
    state_topic_ = preview.state;
    state_topic_size_ = std::strlen(state_topic_.data());
    result_topic_ = preview.result;
    result_topic_size_ = std::strlen(result_topic_.data());
    command_prefix_ = preview.command;
    command_prefix_size_ = std::strlen(command_prefix_.data());
    filters_ = {};
    filter_count_ = 0U;
    const auto add = [&](const std::string_view root) {
        if (root.empty())
            return true;
        std::array<char, kTopicCapacity> filter{};
        if (!append_suffix(root, "#", filter))
            return false;
        for (std::size_t i = 0; i < filter_count_; ++i)
            if (std::string_view{filters_[i].data()} == std::string_view{filter.data()})
                return true;
        if (filter_count_ >= filters_.size())
            return false;
        filters_[filter_count_++] = filter;
        return true;
    };
    if (!add(preview.command.data()))
        return false;
    for (const auto& group : preview.groups)
        if (!add(group.data()))
            return false;
    if (!add(preview.fallback.data()))
        return false;
    command_filter_ = filters_[0];
    command_filter_size_ = std::strlen(command_filter_.data());
    if (!build_sensor_topic())
        return false;
    announce_stage_ = 0U;
    power_cursor_ = 0U;
    last_power_revision_ = 0U;
    was_connected_ = false;
    cleanup_active_ = cleanup_finished_ = false;
    return true;
}

void MqttService::set_response_command(const std::string_view command) {
    response_command_.fill(0);
    for (std::size_t i = 0; i < command.size() && i + 1U < response_command_.size(); ++i)
        response_command_[i] =
            static_cast<char>(std::toupper(static_cast<unsigned char>(command[i])));
    response_fallback_ = last_command_fallback_;
}

void MqttService::set_enabled(const bool enabled) {
    enabled_ = enabled;
    if (!enabled && response_consumer_ != MessageRouter::kInvalidConsumer) {
        (void)messages_.unsubscribe(response_consumer_);
        response_consumer_ = MessageRouter::kInvalidConsumer;
        response_pending_ = false;
        publication_count_ = 0U;
    } else if (enabled && response_consumer_ == MessageRouter::kInvalidConsumer)
        response_consumer_ = messages_.subscribe();
}

bool MqttService::queue_command_result(std::string_view suffix, const std::string_view payload) {
    if (!enabled_)
        return true;
    if (transport_.state == nullptr ||
        transport_.state(transport_.context) != MqttTransportState::connected) {
        ++stats_.result_rejected;
        return false;
    }
    MessageMetadata metadata{};
    metadata.kind = MessageKind::response;
#if USE_TASMOTA_STATE
    suffix = commands::status_response_suffix(payload, suffix);
#endif
    if (!metadata.set_source("command") || !metadata.set_response_topic_suffix(suffix) ||
        messages_.publish(metadata, payload) != MessagePublishResult::published) {
        ++stats_.result_rejected;
        return false;
    }
    return true;
}

bool MqttService::queue_status_event(
    std::string_view suffix, std::string_view payload, bool result_or_stat) {
    if (suffix.empty())
        return false;
    for (const auto c : suffix)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '_'))
            return false;
    if (!result_topic_size_ && !build_command_topics())
        return false;
    const std::string_view result =
        parity_configured_ ? std::string_view{preview_.result.data()}
                           : std::string_view{result_topic_.data(), result_topic_size_};
    const auto cut = result.rfind('/');
    std::array<char, kTopicCapacity> topic{};
    if (cut == std::string_view::npos ||
        !append_suffix(result.substr(0, cut + 1),
            result_or_stat && !config_.response_topic ? std::string_view{"RESULT"} : suffix, topic))
        return false;
    /** Tasmota uppercases the event leaf, preserving the configured prefix/topic. */
    for (auto i = cut + 1; i < topic.size() && topic[i]; ++i)
        if (topic[i] >= 'a' && topic[i] <= 'z')
            topic[i] = char(topic[i] - 'a' + 'A');
    return queue_publish(topic.data(), payload, false);
}

bool MqttService::queue_sensor_event(std::string_view payload) {
    return queue_telemetry_event("SENSOR", payload);
}

bool MqttService::queue_telemetry_event(
    std::string_view suffix, std::string_view payload, const bool retained) {
    if (suffix.empty())
        return false;
    for (const auto c : suffix)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '_'))
            return false;
    if (!sensor_topic_size_ && !build_sensor_topic())
        return false;
    const std::string_view sensor{sensor_topic_.data(), sensor_topic_size_};
    const auto separator = sensor.rfind('/');
    std::array<char, kTopicCapacity> topic{};
    if (separator == std::string_view::npos ||
        !append_suffix(sensor.substr(0, separator + 1), suffix, topic))
        return false;
    return queue_publish(topic.data(), payload, retained);
}

bool MqttService::queue_publish(
    const std::string_view topic, const std::string_view payload, const bool retained) {
    if (!enabled_ || transport_.state == nullptr ||
        transport_.state(transport_.context) != MqttTransportState::connected ||
        publication_count_ == publications_.size() || topic.empty() ||
        topic.size() >= kTopicCapacity || topic.find_first_of("+#\r\n") != std::string_view::npos ||
        topic.find('\0') != std::string_view::npos ||
        payload.size() > CanonicalMessage::kPayloadCapacity ||
        topic.size() + payload.size() + 4U +
                (topic.size() + payload.size() + 2U >= 128U ? 1U : 0U) >
            MqttFrameReader::kMaximumPacketSize)
        return false;
    auto& publication = publications_[publication_count_++];
    publication = {};
    std::memcpy(publication.topic.data(), topic.data(), topic.size());
    std::memcpy(publication.payload.data(), payload.data(), payload.size());
    publication.payload_size = payload.size();
    publication.retained = retained;
    return true;
}

#ifndef NEWMOTA_IMAGE_SAFEBOOT
bool MqttService::build_key_topic(const KeyKind kind, const std::uint8_t instance,
    std::array<char, kTopicCapacity>& topic, const MqttSettings* route) const {
    const auto& settings = route != nullptr ? *route : config_;
    const bool switch_key = kind == KeyKind::switch_input;
    const auto stored = switch_key ? settings.switch_topic() : settings.button_topic();
    if (stored.empty() || stored == "0")
        return false;
    std::array<char, network_identity::kIdentityCapacity> resolved{};
    std::size_t length{};
    if (!network_identity::resolve_topic(stored, platform::device_id(), resolved, length))
        return false;
    char suffix[16]{};
    // Tasmota's SendKey limits button indexes to the Power endpoints and always indexes switches
    // when any endpoint exists (GetPowerDevice option key + SetOption26).
    const auto key = !switch_key && instance > button_endpoint_count_ ? 1U : unsigned(instance);
    const bool indexed = switch_key ? button_endpoint_count_ != 0U || settings.power_index
                                    : button_endpoint_count_ > 1U || settings.power_index;
    std::snprintf(suffix, sizeof suffix, indexed ? "POWER%u" : "POWER", key);
    return resolve_path(settings, 0U, {resolved.data(), length}, hostname_.data(),
        button_identity_.data(), suffix, topic);
}

bool MqttService::route_key(const MqttSettings& current, const KeyKind kind,
    const std::uint8_t instance, const std::uint8_t key_instance, const std::uint8_t key_state,
    const bool on, const bool force_local, const bool no_hold_retain) {
    if (!enabled_ || !current.enabled || !parity_configured_ || transport_.state == nullptr ||
        transport_.publish == nullptr ||
        transport_.state(transport_.context) != MqttTransportState::connected)
        return false;
    const bool switch_key = kind == KeyKind::switch_input;
    const char* const log_name = switch_key ? "SWT: Switch" : "BTN: Button";
    std::array<char, kTopicCapacity> topic{};
    if (!build_key_topic(kind, key_instance, topic, &current))
        return false;
    if (cleanup_active_ || cleanup_finished_ || current != config_) {
        AddLog(LogLevel::warning, "%s%u MQTT key dropped: reconfiguring", log_name,
            unsigned(instance));
        return !force_local;
    }
    std::array<char, network_identity::kIdentityCapacity> resolved{};
    std::size_t length{};
    if (!network_identity::resolve_topic(
            switch_key ? current.switch_topic() : current.button_topic(), platform::device_id(),
            resolved, length))
        return false;
    const std::string_view key_topic{resolved.data(), length};
    auto state = unsigned(key_state);
    if (state == 2U &&
        (force_local || key_topic == std::string_view{base_topic_.data(), base_topic_size_} ||
            key_topic == current.group_topic(0U)))
        state = on ? 0U : 1U;
    const bool retain = switch_key ? current.switch_retain : current.button_retain;
    // This is an intended command, including a command to ourselves. Do not register it
    // as a response/telemetry echo when the configured command/status prefixes coincide.
    if (!transport_.publish(transport_.context, topic.data(), state_text(state),
            retain && !current.no_retain && (key_state != 3U || !no_hold_retain))) {
        ++stats_.failed;
        AddLog(
            LogLevel::warning, "%s%u MQTT key dropped: writer busy", log_name, unsigned(instance));
    } else {
        ++stats_.published;
    }
    // Tasmota remote policy consumes the route even when the nonblocking writer is busy.
    return !force_local;
}

bool MqttService::route_button_key(const MqttSettings& current, const ButtonEvent& event,
    const bool on, const bool force_local, const bool no_hold_retain) {
    return event.send_key && route_key(current, KeyKind::button, event.instance, event.key_instance,
                                 event.state, on, force_local, no_hold_retain);
}

#if USE_SWITCHES
bool MqttService::route_switch_key(const MqttSettings& current, const SwitchEvent& event,
    const bool on, const bool force_local, const bool no_hold_retain) {
    return route_key(current, KeyKind::switch_input, event.instance, event.instance, event.state,
        on, force_local, no_hold_retain);
}
#endif

bool MqttService::queue_button_action(std::string_view suffix, std::string_view payload) {
    if (!queue_status_event(suffix, payload, true))
        return false;
    publications_[publication_count_ - 1U].session_scoped = true;
    return true;
}

void MqttService::discard_session_publications() {
    std::size_t kept{};
    for (std::size_t i = 0U; i < publication_count_; ++i)
        if (!publications_[i].session_scoped) {
            if (kept != i)
                publications_[kept] = publications_[i];
            ++kept;
        }
    publication_count_ = kept;
}

void MqttService::request_button_retain_clear(const std::uint32_t now) {
    request_key_clear(KeyKind::button, now);
}

#if USE_SWITCHES
void MqttService::request_switch_retain_clear(const std::uint32_t now) {
    request_key_clear(KeyKind::switch_input, now);
}
#endif

void MqttService::request_key_clear(const KeyKind kind, const std::uint32_t now) {
    const auto stored =
        kind == KeyKind::switch_input ? config_.switch_topic() : config_.button_topic();
    if (!config_.no_retain && !stored.empty() && stored != "0") {
        key_clears_[static_cast<std::size_t>(kind)] = {1U, now};
    }
}

std::uint8_t MqttService::key_count(const KeyKind kind) const {
    const auto count = kind == KeyKind::switch_input ? switch_count_ : button_endpoint_count_;
    return count ? count : 1U;
}

bool MqttService::cleanup_key_topic(std::array<char, kTopicCapacity>& topic) {
    const unsigned first = 22U + power_count_;
    const unsigned buttons = key_count(KeyKind::button);
    if (cleanup_cursor_ < first + buttons) {
        if (config_.button_retain &&
            build_key_topic(
                KeyKind::button, static_cast<std::uint8_t>(cleanup_cursor_ - first + 1U), topic))
            return true;
        cleanup_cursor_ = static_cast<decltype(cleanup_cursor_)>(first + buttons);
    }
    const unsigned switches = first + buttons;
    return cleanup_cursor_ < switches + key_count(KeyKind::switch_input) && config_.switch_retain &&
           build_key_topic(KeyKind::switch_input,
               static_cast<std::uint8_t>(cleanup_cursor_ - switches + 1U), topic);
}

bool MqttService::publish_key_clear(const std::uint32_t now) {
    for (std::size_t index = 0U; index < key_clears_.size(); ++index) {
        auto& clear = key_clears_[index];
        if (clear.cursor == 0U)
            continue;
        if (now - clear.started >= 5000U) {
            clear.cursor = 0U;
            ++stats_.cleanup_failures;
            continue;
        }
        const auto kind = static_cast<KeyKind>(index);
        std::array<char, kTopicCapacity> topic{};
        if (!build_key_topic(kind, clear.cursor, topic)) {
            clear.cursor = 0U;
            continue;
        }
        if (transport_.publish(transport_.context, topic.data(), {}, true)) {
            ++stats_.published;
            if (++clear.cursor > key_count(kind))
                clear.cursor = 0U;
        }
        return true;
    }
    return false;
}
#endif

bool MqttService::send(
    const std::string_view topic, const std::string_view payload, const bool retained) {
    const bool accepted =
        transport_.publish(transport_.context, topic, payload, retained && !config_.no_retain);
    if (accepted &&
        (config_.prefix(0) == config_.prefix(1) || config_.prefix(0) == config_.prefix(2))) {
        echo_hashes_[echo_cursor_++ % echo_hashes_.size()] = publication_hash(topic, payload);
    }
    return accepted;
}

/** @brief Validates and copies one bounded Tasmota base topic. */
bool MqttService::set_topic(const std::string_view topic) {
    if (topic.empty() || topic.size() >= base_topic_.size() || topic.front() == '/' ||
        topic.back() == '/' || has_wildcard(topic)) {
        return false;
    }
    base_topic_.fill('\0');
    std::memcpy(base_topic_.data(), topic.data(), topic.size());
    base_topic_size_ = topic.size();
    state_topic_size_ = 0U;
    result_topic_size_ = 0U;
    command_prefix_size_ = 0U;
    command_filter_size_ = 0U;
    has_published_ = false;
    sensor_has_published_ = false;
    response_pending_ = false;
    const bool configured = parity_configured_;
    parity_configured_ = false;
    const bool valid = build_state_topic() && build_sensor_topic() && build_command_topics();
    parity_configured_ = configured;
    return valid;
}

/** @brief Applies one validated shared telemetry interval. */
bool MqttService::set_tele_period_seconds(const std::uint16_t seconds) {
    if (!TelemetrySettings::valid_period(seconds))
        return false;
    tele_period_seconds_ = seconds;
    return true;
}

/** @brief Builds the exact first-slice Tasmota telemetry topic. */
bool MqttService::build_state_topic() {
    constexpr std::string_view prefix = "tele/";
    constexpr std::string_view suffix = "/STATE";
    return build_topic(
        state_topic_, state_topic_size_, prefix, {base_topic_.data(), base_topic_size_}, suffix);
}

/** @brief Builds the exact first-slice Tasmota sensor telemetry topic. */
bool MqttService::build_sensor_topic() {
    if (!parity_configured_) {
        return build_topic(sensor_topic_, sensor_topic_size_, "tele/",
            {base_topic_.data(), base_topic_size_}, "/SENSOR");
    }
    const std::string_view state{preview_.state.data()};
    const auto separator = state.rfind('/');
    if (separator == std::string_view::npos)
        return false;
    if (!append_suffix(state.substr(0U, separator + 1U), "SENSOR", sensor_topic_))
        return false;
    sensor_topic_size_ = std::strlen(sensor_topic_.data());
    return true;
}

/** @brief Builds the MQTT command and result topic paths. */
bool MqttService::build_command_topics() {
    const std::string_view base{base_topic_.data(), base_topic_size_};
    return build_topic(result_topic_, result_topic_size_, "stat/", base, "/RESULT") &&
           build_topic(command_prefix_, command_prefix_size_, "cmnd/", base, "/") &&
           build_topic(command_filter_, command_filter_size_, "cmnd/", base, "/#");
}

/** @brief Extracts one command suffix from an MQTT command topic. */
bool MqttService::command_topic(const std::string_view topic, std::string_view& command) {
    command = {};
    last_command_group_ = last_command_fallback_ = false;
    const auto match = [&](const std::string_view root) {
        if (root.empty() || topic.size() <= root.size() || topic.substr(0, root.size()) != root)
            return false;
        command = topic.substr(root.size());
        return command.find_first_of("/# +;\r\n\t") == std::string_view::npos &&
               command.find('\0') == std::string_view::npos;
    };
    if (parity_configured_ && config_.prefix(0) == config_.prefix(1)) {
        // Result payloads are JSON; the bridge additionally rejects JSON echoes.
    }
    if (match({command_prefix_.data(), command_prefix_size_}))
        return true;
    if (parity_configured_) {
        for (const auto& root : preview_.groups)
            if (match(root.data())) {
                last_command_group_ = true;
                return true;
            }
        if (match(preview_.fallback.data())) {
            last_command_fallback_ = true;
            return true;
        }
    }
    return false;
}

/** @brief Copies one complete router payload into service-owned storage. */
bool MqttService::copy_snapshot(const CanonicalMessage& snapshot) {
    const auto payload = snapshot.payload();
    if (!snapshot.valid() || payload.empty() || payload.size() > payload_.size()) {
        return false;
    }
    std::memcpy(payload_.data(), payload.data(), payload.size());
    payload_size_ = payload.size();
    return true;
}

/** @brief Copies one complete ordered result message into owned storage. */
bool MqttService::copy_response(const CanonicalMessage& response) {
    const auto payload = response.payload();
    const auto source = response.metadata().source();
    set_response_command(response.metadata().response_topic_suffix());
    response_fallback_ = response.metadata().mqtt_fallback;
    if (!response.valid() || response.metadata().kind != MessageKind::response ||
        (source != "mqtt" && source != "wifi" && source != "command" && source != "ping") ||
        payload.empty() || payload.size() > response_payload_.size()) {
        return false;
    }
    response_is_wifi_event_ = source == "wifi";
    if (parity_configured_) {
        std::string_view suffix = "RESULT";
        if (!response_is_wifi_event_ && config_.response_topic && response_command_[0])
            suffix = response_command_.data();
        if (payload.substr(0U, 8U) == "{\"Status") {
            const auto end = payload.find('"', 2U);
            if (end != std::string_view::npos) {
                const auto key = payload.substr(2U, end - 2U);
                if (key == "Status")
                    suffix = payload.find(",\"StatusPRM\":") == std::string_view::npos ? "STATUS"
                                                                                       : "STATUS0";
                else if (key == "StatusPRM")
                    suffix = "STATUS1";
                else if (key == "StatusFWR")
                    suffix = "STATUS2";
                else if (key == "StatusLOG")
                    suffix = "STATUS3";
                else if (key == "StatusMEM")
                    suffix = "STATUS4";
                else if (key == "StatusNET")
                    suffix = "STATUS5";
                else if (key == "StatusMQT")
                    suffix = "STATUS6";
                else if (key == "StatusTIM")
                    suffix = "STATUS7";
                else if (key == "StatusSNS")
#if USE_TASMOTA_STATE
                    suffix = response_command_.data();
#else
                    suffix = "STATUS10";
#endif
                else if (key == "StatusPTH")
                    suffix = "STATUS9";
                else if (key == "StatusSTS")
                    suffix = "STATUS11";
            }
        }
        const std::string_view original{preview_.result.data()};
        const auto cut = original.rfind('/');
        if (!append_suffix(original.substr(0U, cut + 1U), suffix, result_topic_))
            return false;
        result_topic_size_ = std::strlen(result_topic_.data());
        if (response_fallback_) {
            const auto n =
                std::snprintf(result_topic_.data(), result_topic_.size(), "stat/%s_fb/%.*s",
                    resolved_client_.data(), static_cast<int>(suffix.size()), suffix.data());
            if (n < 0 || static_cast<std::size_t>(n) >= result_topic_.size())
                return false;
            result_topic_size_ = static_cast<std::size_t>(n);
        }
    }
    std::memcpy(response_payload_.data(), payload.data(), payload.size());
    response_payload_size_ = payload.size();
    return true;
}

/** @brief Publishes at most one newest power snapshot without blocking. */
void MqttService::poll(const std::uint32_t now) {
    if (!enabled_)
        return;
    if (transport_.state == nullptr || transport_.publish == nullptr) {
        was_connected_ = false;
        sensor_has_published_ = false;
        ++stats_.unavailable;
        return;
    }
    const auto state = transport_.state(transport_.context);
    if (state == MqttTransportState::disconnected || state == MqttTransportState::unavailable) {
#ifndef NEWMOTA_IMAGE_SAFEBOOT
        discard_session_publications();
#endif
        if (response_consumer_ != MessageRouter::kInvalidConsumer) {
            (void)messages_.unsubscribe(response_consumer_);
            response_consumer_ = MessageRouter::kInvalidConsumer;
        }
        was_connected_ = false;
        sensor_has_published_ = false;
        return;
    }
    if (response_consumer_ == MessageRouter::kInvalidConsumer)
        response_consumer_ = messages_.subscribe();
    if (state != MqttTransportState::connected) {
#ifndef NEWMOTA_IMAGE_SAFEBOOT
        discard_session_publications();
#endif
        was_connected_ = false;
        sensor_has_published_ = false;
        return;
    }

    if (parity_configured_) {
        if (cleanup_active_) {
            (void)publish_cleanup();
            return;
        }
        if (cleanup_finished_)
            return;
        if (!was_connected_) {
            announce_stage_ = 0U;
            power_cursor_ = 0U;
            has_published_ = false;
            sensor_has_published_ = false;
            last_power_revision_ = 0U;
            was_connected_ = true;
        }
        if (announce_stage_ < 4U) {
            (void)publish_announcements();
            return;
        }
    }
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    if (publish_key_clear(now))
        return;
#endif
    if (publication_count_ != 0U) {
        const auto& publication = publications_[0];
        if (send(publication.topic.data(), {publication.payload.data(), publication.payload_size},
                publication.retained)) {
            if (publication_count_ > 1U)
                publications_[0] = publications_[1];
            --publication_count_;
            ++stats_.published;
        } else
            ++stats_.failed;
        return;
    }
    if (!response_pending_ && response_consumer_ != MessageRouter::kInvalidConsumer) {
        const auto* const response = messages_.next_ordered(response_consumer_);
        if (response != nullptr && copy_response(*response)) {
            response_pending_ = true;
        } else if (response != nullptr) {
            ++stats_.invalid;
        }
    }
    if (response_pending_) {
        if (result_topic_size_ == 0U ||
            !send({result_topic_.data(), result_topic_size_},
                {response_payload_.data(), response_payload_size_},
                config_.status_retain &&
                    std::string_view{result_topic_.data(), result_topic_size_}.find("/STATUS") !=
                        std::string_view::npos)) {
            ++stats_.failed;
            was_connected_ = true;
            return;
        }
        response_pending_ = false;
        ++stats_.published;
        if (response_is_wifi_event_) {
            ++stats_.wifi_events_published;
        } else {
            ++stats_.responses_published;
        }
        was_connected_ = true;
        return;
    }

    const auto* const power_snapshot = messages_.latest_snapshot("power");
    if (power_snapshot != nullptr) {
        if (parity_configured_ && publish_power(*power_snapshot))
            return;

        const auto revision = power_snapshot->metadata().revision;
        const bool connection_start = !was_connected_;
        const bool state_changed = !has_published_ || published_revision_ != revision;
        const auto period_milliseconds = static_cast<std::uint32_t>(tele_period_seconds_) * 1000U;
        const bool periodic_due =
            tele_period_seconds_ != 0U &&
            (!has_published_ || now - last_published_at_ >= period_milliseconds);
        if (connection_start || state_changed || periodic_due || publish_requested_) {
            if ((!parity_configured_ && !build_state_topic()) || !copy_snapshot(*power_snapshot)) {
                ++stats_.invalid;
                was_connected_ = true;
                return;
            }

            if (!send({state_topic_.data(), state_topic_size_}, {payload_.data(), payload_size_},
                    config_.state_retain)) {
                ++stats_.failed;
                was_connected_ = true;
                return;
            }
            published_revision_ = revision;
            last_published_at_ = now;
            has_published_ = true;
            publish_requested_ = false;
            was_connected_ = true;
            ++stats_.published;
            return;
        }
    }

    const auto* const sensor_snapshot = messages_.latest_snapshot("sensors");
    if (sensor_snapshot != nullptr && publish_sensor(*sensor_snapshot, now))
        return;
    was_connected_ = true;
}

/** @brief Publishes one canonical sensor snapshot when its cadence is due. */
bool MqttService::publish_sensor(const CanonicalMessage& snapshot, const std::uint32_t now) {
    auto payload = snapshot.payload();
    if (!snapshot.valid() || snapshot.metadata().source() != "sensors" || payload.empty()) {
        ++stats_.invalid;
        return true;
    }
    const auto period_milliseconds = static_cast<std::uint32_t>(tele_period_seconds_) * 1000U;
    const bool connection_start = !was_connected_;
    const bool periodic_due =
        tele_period_seconds_ != 0U &&
        (!sensor_has_published_ || now - sensor_last_published_at_ >= period_milliseconds);
    if (!connection_start && sensor_has_published_ && !periodic_due && !sensor_requested_)
        return false;
    if ((!parity_configured_ && !build_sensor_topic()) || sensor_topic_size_ == 0U) {
        ++stats_.invalid;
        return true;
    }
    const auto provider = sensor_document_;
    std::uint32_t token{};
    if (provider.prepare) {
        std::size_t size{};
        if (!provider.prepare(provider.context, payload_.data(), payload_.size(), size, token) ||
            !size || size > CanonicalMessage::kPayloadCapacity) {
            ++stats_.failed;
            return true;
        }
        payload = {payload_.data(), size};
    }
    // SensorRetain is not implemented; StateRetain applies only to STATE.
    if (!send({sensor_topic_.data(), sensor_topic_size_}, payload, false)) {
        ++stats_.failed;
        return true;
    }
    if (provider.accepted)
        provider.accepted(provider.context, token);
    sensor_last_published_at_ = now;
    sensor_has_published_ = true;
    sensor_requested_ = false;
    was_connected_ = true;
    ++stats_.published;
    return true;
}

void MqttService::set_connection_info(const std::string_view version,
    const std::string_view address, const RestartReasonText restart_reason,
    const std::uint32_t boot_count) {
    version_.fill(0);
    address_.fill(0);
    if (version.size() < version_.size())
        std::memcpy(version_.data(), version.data(), version.size());
    if (address.size() < address_.size())
        std::memcpy(address_.data(), address.data(), address.size());
    restart_reason_ = restart_reason;
    boot_count_ = boot_count;
}

bool MqttService::consume_echo(
    const std::string_view topic, const std::string_view payload, const bool retained) {
    if (retained)
        return false;
    const auto hash = publication_hash(topic, payload);
    for (auto& echo : echo_hashes_)
        if (echo == hash) {
            echo = 0U;
            return true;
        }
    return false;
}

bool MqttService::publish_announcements() {
    std::array<char, kTopicCapacity> topic{};
    std::array<char, 512> payload{};
    std::size_t size = 0U;
    std::string_view body;
    bool retained = config_.info_retain;
    if (announce_stage_ == 0U) {
        topic = preview_.lwt;
        body = "Online";
        retained = true;
    } else {
        const std::string_view state{preview_.state.data()};
        const auto cut = state.rfind('/');
        const char* suffix = announce_stage_ == 1U   ? "INFO1"
                             : announce_stage_ == 2U ? "INFO2"
                                                     : "INFO3";
        if (!append_suffix(state.substr(0, cut + 1U), suffix, topic))
            return false;
        bool valid = false;
        if (announce_stage_ == 1U) {
            std::array<char, 70> fallback{};
            std::snprintf(fallback.data(), fallback.size(), "%s_fb", resolved_client_.data());
            valid = append_json(payload, size, "{\"Info1\":{\"Module\":\"Newmota\",\"Version\":") &&
                    append_json(payload, size, version_.data(), true) &&
                    append_json(payload, size, ",\"FallbackTopic\":") &&
                    append_json(payload, size, fallback.data(), true) &&
                    append_json(payload, size, ",\"GroupTopic\":") &&
                    append_json(payload, size, config_.group_topic(0), true) &&
                    append_json(payload, size, "}}");
        } else if (announce_stage_ == 2U) {
            valid = append_json(payload, size, "{\"Info2\":{\"Hostname\":") &&
                    append_json(payload, size, hostname_.data(), true) &&
                    append_json(payload, size, ",\"IPAddress\":") &&
                    append_json(payload, size, address_.data(), true) &&
                    append_json(payload, size, "}}");
        } else {
            char count[16]{};
            std::snprintf(count, sizeof count, "%lu", static_cast<unsigned long>(boot_count_));
            valid = append_json(payload, size, "{\"Info3\":{\"RestartReason\":") &&
                    append_json(payload, size,
#if USE_TASMOTA_STATE
                        restart_reason_,
#else
                        restart_reason_ ? "Power On" : "Restart",
#endif
                        true) &&
                    append_json(payload, size, ",\"BootCount\":") &&
                    append_json(payload, size, count) && append_json(payload, size, "}}");
        }
        if (!valid) {
            ++stats_.invalid;
            return false;
        }
        body = {payload.data(), size};
    }
    if (!send(topic.data(), body, retained)) {
        ++stats_.failed;
        return false;
    }
    ++announce_stage_;
    ++stats_.published;
    return true;
}

bool MqttService::publish_power(const CanonicalMessage& snapshot) {
    const auto revision = snapshot.metadata().revision;
    if (power_cursor_ == 0U && last_power_revision_ == revision && has_published_)
        return false;
    const auto body = snapshot.payload();
    if (power_cursor_ == 0U) {
        pending_power_mask_ = 0U;
#if USE_STATE_TEXT
        const bool typed_power = power_snapshot_ != nullptr;
        if (typed_power && !power_snapshot_(power_snapshot_context_, revision, pending_power_mask_))
            return false;
#else
        constexpr bool typed_power = false;
#endif
        power_count_ = 0U;
        power_present_mask_ = 0U;
        power_json_sent_ = false;
        for (std::size_t start = 0U;;) {
            const auto pos = body.find("\"POWER", start);
            if (pos == std::string_view::npos)
                break;
            const auto end = body.find('"', pos + 1U);
            if (end == std::string_view::npos)
                break;
            const auto key = body.substr(pos + 1U, end - pos - 1U);
            unsigned index = 1U;
            if (key.size() > 5U) {
                index = 0U;
                for (auto c : key.substr(5U)) {
                    if (c < '0' || c > '9') {
                        index = 0U;
                        break;
                    }
                    index = index * 10U + static_cast<unsigned>(c - '0');
                }
            }
            if (index > 0U && index <= 32U) {
                power_present_mask_ |= 1U << (index - 1U);
                power_count_ =
                    static_cast<std::uint8_t>(index > power_count_ ? index : power_count_);
                const auto value = body.find('"', end + 1U);
                if (value == std::string_view::npos)
                    return false;
                if (!typed_power) {
                    /** Unbound consumers cannot infer truth from identical presentation words. */
                    if (state_text(0) == state_text(1))
                        return false;
                    std::array<char, 65> on_word{}, off_word{};
                    std::size_t on_size{}, off_size{};
                    if (!append_json(on_word, on_size, state_text(1), true) ||
                        !append_json(off_word, off_size, state_text(0), true))
                        return false;
                    if (body.substr(value, on_size) == std::string_view{on_word.data(), on_size})
                        pending_power_mask_ |= 1U << (index - 1U);
                    else if (body.substr(value, off_size) !=
                             std::string_view{off_word.data(), off_size})
                        /** A pre-alias-change snapshot must refresh; never infer OFF. */
                        return false;
                }
                /** Skip the complete escaped value so alias text cannot masquerade as a Power key.
                 */
                auto value_end = value + 1;
                for (; value_end < body.size(); ++value_end) {
                    if (body[value_end] == '\\') {
                        ++value_end;
                        continue;
                    }
                    if (body[value_end] == '"')
                        break;
                }
                start = value_end + 1;
                continue;
            }
            start = end + 1U;
        }
        power_cursor_ = 1U;
    }
    while (power_cursor_ <= power_count_) {
        const auto index = power_cursor_;
        const auto bit = 1U << (index - 1U);
        if ((power_present_mask_ & bit) == 0U) {
            ++power_cursor_;
            continue;
        }
        if (has_published_ && last_power_revision_ != 0U &&
            (pending_power_mask_ & bit) == (power_mask_ & bit)) {
            ++power_cursor_;
            continue;
        }
        std::array<char, kTopicCapacity> topic{};
        char suffix[16]{};
        std::snprintf(suffix, sizeof suffix,
            (power_count_ == 1U && !config_.power_index) ? "POWER" : "POWER%u",
            static_cast<unsigned>(index));
        const std::string_view result{preview_.result.data()};
        if (!append_suffix(result.substr(0, result.rfind('/') + 1U), suffix, topic))
            return true;
        const auto value = state_text(bool(pending_power_mask_ & bit));
        if (!power_json_sent_) {
            std::array<char, 96> json{};
            std::size_t json_size{};
            if (!append_json(json, json_size, "{") || !append_json(json, json_size, suffix, true) ||
                !append_json(json, json_size, ":") || !append_json(json, json_size, value, true) ||
                !append_json(json, json_size, "}"))
                return true;
            std::array<char, kTopicCapacity> result_topic{};
            if (!append_suffix(result.substr(0, result.rfind('/') + 1U),
                    config_.response_topic ? std::string_view{suffix} : std::string_view{"RESULT"},
                    result_topic))
                return true;
            if (!send(result_topic.data(), {json.data(), json_size}, false)) {
                ++stats_.failed;
                return true;
            }
            power_json_sent_ = true;
            return true;
        }
        if (!config_.only_json && !send(topic.data(), value, config_.power_retain)) {
            ++stats_.failed;
            return true;
        }
        ++power_cursor_;
        power_json_sent_ = false;
        ++stats_.published;
        return true;
    }
    power_cursor_ = 0U;
    power_mask_ = pending_power_mask_;
    last_power_revision_ = revision;
    return false;
}

bool MqttService::prepare_reconfigure(const MqttSettings& next, const std::uint32_t now) {
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    discard_session_publications();
    for (auto& clear : key_clears_) {
        if (clear.cursor == 0U)
            continue;
        if (now - clear.started < 5000U)
            return false;
        clear.cursor = 0U;
        ++stats_.cleanup_failures;
    }
#endif
    if (!parity_configured_ || cleanup_finished_ || config_.no_retain)
        return true;
    const bool routes_changed =
        config_.topic() != next.topic() || config_.full_topic() != next.full_topic() ||
        config_.prefix_storage != next.prefix_storage || config_.client_id() != next.client_id() ||
        config_.power_index != next.power_index || config_.enabled != next.enabled ||
        config_.host() != next.host() || config_.port != next.port ||
        config_.no_retain != next.no_retain
#ifndef NEWMOTA_IMAGE_SAFEBOOT
        || config_.button_topic() != next.button_topic() ||
        config_.switch_topic() != next.switch_topic()
#endif
        ;
    const bool retain_changed = (config_.power_retain && !next.power_retain) ||
                                (config_.info_retain && !next.info_retain) ||
                                (config_.state_retain && !next.state_retain) ||
                                (config_.status_retain && !next.status_retain) ||
                                (!config_.no_retain && next.no_retain)
#ifndef NEWMOTA_IMAGE_SAFEBOOT
                                || (config_.button_retain && !next.button_retain) ||
                                (config_.switch_retain && !next.switch_retain)
#endif
        ;
    if (!routes_changed && !retain_changed)
        return true;
    if (!cleanup_active_) {
        cleanup_active_ = true;
        cleanup_cursor_ = 0U;
        cleanup_started_ = now;
    }
    if (now - cleanup_started_ >= 5000U) {
        cleanup_active_ = false;
        cleanup_finished_ = false;
        ++stats_.failed;
        ++stats_.cleanup_failures;
        return true;
    }
    return false;
}

bool MqttService::publish_cleanup() {
    std::array<char, kTopicCapacity> topic{};
    bool retained = true;
    if (cleanup_cursor_ == 0U) {
        topic = preview_.lwt;
    } else {
        const std::string_view result{preview_.result.data()};
        const std::string_view state{preview_.state.data()};
        char suffix[24]{};
        if (cleanup_cursor_ <= 3U) {
            std::snprintf(suffix, sizeof suffix, "INFO%u", cleanup_cursor_);
            if (!append_suffix(state.substr(0, state.rfind('/') + 1U), suffix, topic))
                return false;
        } else if (cleanup_cursor_ == 4U) {
            topic = preview_.state;
        } else if (cleanup_cursor_ == 5U) {
            topic = sensor_topic_;
        } else if (cleanup_cursor_ == 6U) {
            if (!append_suffix(result.substr(0, result.rfind('/') + 1U), "STATE", topic))
                return false;
        } else if (cleanup_cursor_ == 7U) {
            if (!append_suffix(result.substr(0, result.rfind('/') + 1U), "STATUS", topic))
                return false;
        } else if (cleanup_cursor_ <= 21U) {
            std::snprintf(suffix, sizeof suffix, "STATUS%u", cleanup_cursor_ - 8U);
            if (!append_suffix(result.substr(0, result.rfind('/') + 1U), suffix, topic))
                return false;
        } else if (cleanup_cursor_ <= 21U + power_count_) {
            std::snprintf(suffix, sizeof suffix,
                (power_count_ == 1U && !config_.power_index) ? "POWER" : "POWER%u",
                cleanup_cursor_ - 21U);
            if (!append_suffix(result.substr(0, result.rfind('/') + 1U), suffix, topic))
                return false;
        }
#ifndef NEWMOTA_IMAGE_SAFEBOOT
        else if (cleanup_key_topic(topic)) {
            // Empty retained command publications use the old Button/SwitchTopic and indexing.
        }
#endif
        else {
            cleanup_active_ = false;
            cleanup_finished_ = true;
            return true;
        }
    }
    // Clearing retained records must bypass the new no-retain policy.
    const auto payload = cleanup_cursor_ == 0U && config_.offline_lwt ? std::string_view{"Offline"}
                                                                      : std::string_view{};
    if (!transport_.publish(transport_.context, topic.data(), payload, retained)) {
        ++stats_.failed;
        return false;
    }
    ++cleanup_cursor_;
    ++stats_.published;
    return true;
}

} // namespace newmota
