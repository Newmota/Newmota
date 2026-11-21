/*
  mqtt_commands.cpp - bounded MQTT command routing for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.
*/

#include "services/mqtt/mqtt_commands.h"
#include "services/status/status_commands.h"

#include "services/mqtt/mqtt_service.h"
#include "support/text.h"

#include <cstdio>
#include <cstring>

namespace newmota {
namespace {

constexpr std::string_view kInvalidArguments = "{\"Command\":\"Invalid arguments\"}";
constexpr std::string_view kFailedResponse = "{\"Command\":\"Failed\"}";

/** @brief Reports whether a command payload contains forbidden line bytes. */
bool invalid_payload(const MqttPublishPacket& packet) {
    if (packet.payload_size != 0U && packet.payload == nullptr)
        return true;
    if (packet.payload == nullptr)
        return false;
    for (std::size_t index = 0U; index < packet.payload_size; ++index) {
        if (packet.payload[index] == 0U || packet.payload[index] == '\r' ||
            packet.payload[index] == '\n') {
            return true;
        }
    }
    return false;
}

bool group_segment_allowed(void*, const std::string_view segment) {
    std::array<char, kMaximumCommandSegmentLength> decoded{};
    std::size_t size = 0U;
    if (!decode_command_segment(segment, decoded.data(), decoded.size(), size))
        return false;
    auto text = support::trim_ascii({decoded.data(), size});
    if (!text.empty() && text.front() == '_')
        text = support::trim_ascii(text.substr(1U));
    const auto separator = text.find_first_of(" \t");
    const auto name = text.substr(0U, separator);
    return !support::ascii_iequals(name, "topic") && !support::ascii_iequals(name, "mqtttopic") &&
           !support::ascii_iequals(name, "mqttclient") &&
           !support::ascii_iequals(name, "buttontopic");
}

} // namespace

MqttCommandService::MqttCommandService(
    MessageRouter& messages, CommandProcessor& commands, MqttService& mqtt)
    : messages_(messages), commands_(commands), mqtt_(mqtt) {}

/** @brief Adapts the client's short-lived PUBLISH view to this service. */
void MqttCommandService::receive_publish(void* const context, const MqttPublishPacket& packet) {
    if (context == nullptr)
        return;
    static_cast<MqttCommandService*>(context)->handle_publish(packet);
}

/** @brief Appends one response fragment without partial successful output. */
bool MqttCommandService::append_response(void* const context, const std::string_view text) {
    auto& service = *static_cast<MqttCommandService*>(context);
    if (text.size() > service.response_.size() - service.response_size_) {
        return false;
    }
    std::memcpy(service.response_.data() + service.response_size_, text.data(), text.size());
    service.response_size_ += text.size();
    return true;
}

/** @brief Copies one MQTT topic command and its text payload. */
bool MqttCommandService::build_command(
    const std::string_view command, const MqttPublishPacket& packet) {
    if (invalid_payload(packet) || command.empty() ||
        command.find('\0') != std::string_view::npos || command.size() >= command_.size()) {
        return false;
    }
    const auto separator = packet.payload_size == 0U ? 0U : 1U;
    if (command.size() + separator + packet.payload_size > command_.size()) {
        return false;
    }
    std::memcpy(command_.data(), command.data(), command.size());
    command_size_ = command.size();
    if (separator != 0U) {
        command_[command_size_++] = ' ';
        std::memcpy(command_.data() + command_size_, packet.payload, packet.payload_size);
        command_size_ += packet.payload_size;
    }
    return true;
}

/** @brief Publishes one command result as a canonical ordered response. */
bool MqttCommandService::publish_response(const std::string_view payload) {
    MessageMetadata metadata{};
    metadata.kind = MessageKind::response;
    if (!metadata.set_source("mqtt")) {
        ++stats_.invalid;
        return false;
    }
#if USE_TASMOTA_STATE
    (void)metadata.set_response_topic_suffix(
        commands::status_response_suffix(payload, response_suffix_.data()));
#else
    (void)metadata.set_response_topic_suffix(response_suffix_.data());
#endif
    metadata.mqtt_fallback = response_fallback_;
    const auto result = messages_.publish(metadata, payload);
    if (result == MessagePublishResult::queue_full) {
        ++stats_.response_queue_full;
        return false;
    } else if (result == MessagePublishResult::invalid) {
        ++stats_.invalid;
        return false;
    }
    return true;
}

/** @brief Routes one validated inbound MQTT command. */
void MqttCommandService::handle_publish(const MqttPublishPacket& packet) {
    std::string_view command;
    if (!mqtt_.command_topic(packet.topic, command)) {
        ++stats_.ignored;
        return;
    }
    if (mqtt_.consume_echo(packet.topic,
            {reinterpret_cast<const char*>(packet.payload), packet.payload_size},
            packet.retained)) {
        ++stats_.ignored;
        return;
    }
    response_suffix_.fill(0);
    if (command.size() < response_suffix_.size())
        std::memcpy(response_suffix_.data(), command.data(), command.size());
    response_fallback_ = mqtt_.command_fallback();
    ++stats_.received;
    if (!build_command(command, packet)) {
        ++stats_.invalid;
        publish_response(kInvalidArguments);
        return;
    }

    std::size_t segment_count = 0U;
    if (!validate_command_line({command_.data(), command_size_}, &segment_count)) {
        ++stats_.invalid;
        publish_response(kInvalidArguments);
        return;
    }
    if (messages_.ordered_slots_available() == 0U) {
        ++stats_.chain_rejected;
        return;
    }
    if (mqtt_.command_group() && !for_each_command_segment({command_.data(), command_size_},
                                     group_segment_allowed, nullptr)) {
        ++stats_.ignored;
        return;
    }
    response_suffix_.fill(0);
    if (command.size() >= response_suffix_.size()) {
        ++stats_.invalid;
        return;
    }
    std::memcpy(response_suffix_.data(), command.data(), command.size());
    response_fallback_ = mqtt_.command_fallback();
    stats_.executed += static_cast<std::uint32_t>(segment_count);
#if USE_TASMOTA_STATE
    commands::set_status_request_fallback(response_fallback_);
#endif
    execute_command();
}

#if USE_TASMOTA_STATE
void MqttCommandService::poll() {
    // Keep a slot free, because a command arriving at a full queue is dropped.
    std::uint8_t section = 0U;
    bool fallback = false;
    if (messages_.ordered_slots_available() < 2U ||
        !commands::next_status_split_section(section, fallback))
        return;
    response_fallback_ = fallback;
    constexpr std::string_view kSuffix = "STATUS";
    response_suffix_.fill(0);
    std::memcpy(response_suffix_.data(), kSuffix.data(), kSuffix.size());
    const auto size = section == 0U ? std::snprintf(command_.data(), command_.size(), "Status")
                                    : std::snprintf(command_.data(), command_.size(), "Status%u",
                                          static_cast<unsigned int>(section));
    command_size_ = static_cast<std::size_t>(size);
    execute_command();
}
#endif

/** @brief Runs the stored command line and publishes its complete response. */
inline void MqttCommandService::execute_command() {
    response_size_ = 0U;
    CommandOutput output{&MqttCommandService::append_response, this};
    const auto result =
        commands_.execute({command_.data(), command_size_}, CommandSource::mqtt, output);
    if (response_size_ == 0U)
        return;
    if (result.output_failed) {
        (void)publish_response(kFailedResponse);
        return;
    }
    std::array<char, CanonicalMessage::kPayloadCapacity> combined{};
    std::size_t size = 0U;
    const std::string_view raw{response_.data(), response_size_};
    if (raw.find("\r\n") == std::string_view::npos) {
        (void)publish_response(raw);
        return;
    }
    const auto append = [&](const std::string_view text) {
        if (size + text.size() > combined.size())
            return false;
        std::memcpy(combined.data() + size, text.data(), text.size());
        size += text.size();
        return true;
    };
    bool valid = append("{\"Results\":[");
    std::size_t pos = 0U, count = 0U;
    while (valid && pos < raw.size()) {
        const auto end = raw.find("\r\n", pos);
        const auto value =
            raw.substr(pos, end == std::string_view::npos ? raw.size() - pos : end - pos);
        if (!value.empty()) {
            if (count++)
                valid = append(",");
            valid = valid && append(value);
        }
        if (end == std::string_view::npos)
            break;
        pos = end + 2U;
    }
    valid = valid && append("]}");
    (void)publish_response(valid ? std::string_view{combined.data(), size} : kFailedResponse);
}

} // namespace newmota
