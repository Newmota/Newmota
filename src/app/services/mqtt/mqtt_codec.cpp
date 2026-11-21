/*
  mqtt_codec.cpp - Newmota's bounded coreMQTT serializer adapter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/mqtt/mqtt_codec.h"

#ifndef MQTT_DO_NOT_USE_CUSTOM_CONFIG
#define MQTT_DO_NOT_USE_CUSTOM_CONFIG 1
#endif

extern "C" {
#include "core_mqtt_serializer.h"
}

#include <cstring>
#include <limits>

namespace newmota {
namespace {

/** @brief Maps a coreMQTT status to Newmota's bounded codec result. */
MqttCodecResult map_status(const MQTTStatus_t status) {
    switch (status) {
    case MQTTSuccess:
        return MqttCodecResult::success;
    case MQTTNoMemory:
        return MqttCodecResult::too_large;
    case MQTTServerRefused:
        return MqttCodecResult::refused;
    case MQTTBadParameter:
    case MQTTSendFailed:
    case MQTTRecvFailed:
    case MQTTBadResponse:
    case MQTTNoDataAvailable:
    case MQTTIllegalState:
    case MQTTStateCollision:
    case MQTTKeepAliveTimeout:
    case MQTTNeedMoreBytes:
        return MqttCodecResult::invalid;
    }
    return MqttCodecResult::invalid;
}

/** @brief Converts one complete frame to coreMQTT's packet view. */
MqttCodecResult packet_info(
    const std::uint8_t* packet, const std::size_t size, MQTTPacketInfo_t& info) {
    if (packet == nullptr || size == 0U) {
        return MqttCodecResult::invalid;
    }
    if (size > MqttCodec::kMaximumPacketSize) {
        return MqttCodecResult::too_large;
    }

    MqttFrameReader framing;
    std::size_t consumed = 0U;
    const auto frame_result = framing.feed(packet, size, consumed);
    if (frame_result == MqttFrameResult::too_large) {
        return MqttCodecResult::too_large;
    }
    if (frame_result != MqttFrameResult::complete || consumed != size) {
        return MqttCodecResult::invalid;
    }

    const std::size_t available = size;
    const auto status = MQTT_ProcessIncomingPacketTypeAndLength(packet, &available, &info);
    const auto result = map_status(status);
    if (result != MqttCodecResult::success || info.headerLength > size ||
        info.remainingLength > size - info.headerLength ||
        info.headerLength + info.remainingLength != size) {
        return MqttCodecResult::invalid;
    }
    info.pRemainingData = const_cast<std::uint8_t*>(packet + info.headerLength);
    return MqttCodecResult::success;
}

/** @brief Reports whether a bounded MQTT string contains a forbidden NUL. */
bool contains_nul(const std::string_view value) {
    return value.find('\0') != std::string_view::npos;
}

/** @brief Maps a fixed-size coreMQTT packet into the caller's buffer. */
template <typename SizeFunction, typename SerializeFunction>
MqttCodecResult serialize_fixed_packet(std::uint8_t* const output, const std::size_t capacity,
    std::size_t& size, SizeFunction get_size, SerializeFunction serialize) {
    size = 0U;
    if (output == nullptr) {
        return MqttCodecResult::invalid;
    }
    std::size_t packet_size = 0U;
    if (get_size(&packet_size) != MQTTSuccess) {
        return MqttCodecResult::invalid;
    }
    if (packet_size > MqttCodec::kMaximumPacketSize || packet_size > capacity) {
        return MqttCodecResult::too_large;
    }
    MQTTFixedBuffer_t buffer{output, capacity};
    const auto status = serialize(&buffer);
    if (status != MQTTSuccess) {
        return map_status(status);
    }
    size = packet_size;
    return MqttCodecResult::success;
}

} // namespace

/** @brief Serializes one canonical JSON publication as MQTT QoS 0. */
MqttCodecResult MqttCodec::serialize_publish(const std::string_view topic,
    const std::string_view payload, const bool retained, std::uint8_t* const output,
    const std::size_t capacity, std::size_t& size) {
    size = 0U;
    if (output == nullptr || topic.empty() ||
        topic.size() > std::numeric_limits<std::uint16_t>::max() ||
        payload.size() > kMaximumPacketSize) {
        return MqttCodecResult::invalid;
    }

    MQTTPublishInfo_t publish{};
    publish.qos = MQTTQoS0;
    publish.retain = retained;
    publish.dup = false;
    publish.pTopicName = topic.data();
    publish.topicNameLength = static_cast<std::uint16_t>(topic.size());
    publish.pPayload = payload.empty() ? nullptr : payload.data();
    publish.payloadLength = payload.size();

    std::size_t remaining_length = 0U;
    std::size_t packet_size = 0U;
    auto status = MQTT_GetPublishPacketSize(&publish, &remaining_length, &packet_size);
    if (status != MQTTSuccess) {
        return map_status(status);
    }
    if (packet_size > kMaximumPacketSize || packet_size > capacity) {
        return MqttCodecResult::too_large;
    }

    MQTTFixedBuffer_t buffer{output, capacity};
    status = MQTT_SerializePublish(&publish, 0U, remaining_length, &buffer);
    if (status != MQTTSuccess) {
        return map_status(status);
    }
    size = packet_size;
    return MqttCodecResult::success;
}

/** @brief Serializes one bounded QoS-0 command subscription. */
MqttCodecResult MqttCodec::serialize_subscribe(const std::string_view topic_filter,
    const std::uint16_t packet_id, std::uint8_t* const output, const std::size_t capacity,
    std::size_t& size) {
    size = 0U;
    if (output == nullptr || topic_filter.empty() || packet_id == 0U ||
        topic_filter.size() > std::numeric_limits<std::uint16_t>::max() ||
        contains_nul(topic_filter)) {
        return MqttCodecResult::invalid;
    }

    MQTTSubscribeInfo_t subscription{};
    subscription.qos = MQTTQoS0;
    subscription.pTopicFilter = topic_filter.data();
    subscription.topicFilterLength = static_cast<std::uint16_t>(topic_filter.size());

    std::size_t remaining_length = 0U;
    std::size_t packet_size = 0U;
    auto status = MQTT_GetSubscribePacketSize(&subscription, 1U, &remaining_length, &packet_size);
    if (status != MQTTSuccess) {
        return map_status(status);
    }
    if (packet_size > kMaximumPacketSize || packet_size > capacity) {
        return MqttCodecResult::too_large;
    }

    MQTTFixedBuffer_t buffer{output, capacity};
    status = MQTT_SerializeSubscribe(&subscription, 1U, packet_id, remaining_length, &buffer);
    if (status != MQTTSuccess) {
        return map_status(status);
    }
    size = packet_size;
    return MqttCodecResult::success;
}

/** @brief Serializes one bounded MQTT UNSUBSCRIBE. */
MqttCodecResult MqttCodec::serialize_unsubscribe(const std::string_view topic_filter,
    const std::uint16_t packet_id, std::uint8_t* const output, const std::size_t capacity,
    std::size_t& size) {
    size = 0U;
    if (output == nullptr || topic_filter.empty() || packet_id == 0U ||
        topic_filter.size() > std::numeric_limits<std::uint16_t>::max() ||
        contains_nul(topic_filter)) {
        return MqttCodecResult::invalid;
    }

    MQTTSubscribeInfo_t subscription{};
    subscription.qos = MQTTQoS0;
    subscription.pTopicFilter = topic_filter.data();
    subscription.topicFilterLength = static_cast<std::uint16_t>(topic_filter.size());

    std::size_t remaining_length = 0U;
    std::size_t packet_size = 0U;
    auto status = MQTT_GetUnsubscribePacketSize(&subscription, 1U, &remaining_length, &packet_size);
    if (status != MQTTSuccess) {
        return map_status(status);
    }
    if (packet_size > kMaximumPacketSize || packet_size > capacity) {
        return MqttCodecResult::too_large;
    }

    MQTTFixedBuffer_t buffer{output, capacity};
    status = MQTT_SerializeUnsubscribe(&subscription, 1U, packet_id, remaining_length, &buffer);
    if (status != MQTTSuccess) {
        return map_status(status);
    }
    size = packet_size;
    return MqttCodecResult::success;
}

/** @brief Serializes one bounded MQTT CONNECT without a last will. */
MqttCodecResult MqttCodec::serialize_connect(const MqttConnectParameters& parameters,
    std::uint8_t* const output, const std::size_t capacity, std::size_t& size) {
    size = 0U;
    if (output == nullptr || parameters.client_id.empty() ||
        parameters.client_id.size() > std::numeric_limits<std::uint16_t>::max() ||
        parameters.username.size() > std::numeric_limits<std::uint16_t>::max() ||
        parameters.password.size() > std::numeric_limits<std::uint16_t>::max() ||
        (!parameters.password.empty() && parameters.username.empty()) ||
        contains_nul(parameters.client_id) || contains_nul(parameters.username) ||
        contains_nul(parameters.password) || contains_nul(parameters.will_topic) ||
        parameters.will_topic.find_first_of("+#") != std::string_view::npos ||
        parameters.will_topic.size() > UINT16_MAX || parameters.will_payload.size() > UINT16_MAX) {
        return MqttCodecResult::invalid;
    }

    MQTTConnectInfo_t connect{};
    connect.cleanSession = parameters.clean_session;
    connect.keepAliveSeconds = parameters.keep_alive_seconds;
    connect.pClientIdentifier = parameters.client_id.data();
    connect.clientIdentifierLength = static_cast<std::uint16_t>(parameters.client_id.size());
    connect.pUserName = parameters.username.empty() ? nullptr : parameters.username.data();
    connect.userNameLength = static_cast<std::uint16_t>(parameters.username.size());
    connect.pPassword = parameters.password.empty() ? nullptr : parameters.password.data();
    connect.passwordLength = static_cast<std::uint16_t>(parameters.password.size());

    std::size_t remaining_length = 0U;
    std::size_t packet_size = 0U;
    MQTTPublishInfo_t will{};
    will.qos = MQTTQoS0;
    will.retain = parameters.will_retained;
    will.pTopicName = parameters.will_topic.data();
    will.topicNameLength = static_cast<std::uint16_t>(parameters.will_topic.size());
    will.pPayload = parameters.will_payload.data();
    will.payloadLength = parameters.will_payload.size();
    const auto* will_ptr = parameters.will_topic.empty() ? nullptr : &will;
    auto status = MQTT_GetConnectPacketSize(&connect, will_ptr, &remaining_length, &packet_size);
    if (status != MQTTSuccess) {
        return map_status(status);
    }
    if (packet_size > kMaximumPacketSize || packet_size > capacity) {
        return MqttCodecResult::too_large;
    }
    MQTTFixedBuffer_t buffer{output, capacity};
    status = MQTT_SerializeConnect(&connect, will_ptr, remaining_length, &buffer);
    if (status != MQTTSuccess) {
        return map_status(status);
    }
    size = packet_size;
    return MqttCodecResult::success;
}

/** @brief Serializes one bounded MQTT PINGREQ. */
MqttCodecResult MqttCodec::serialize_pingreq(
    std::uint8_t* const output, const std::size_t capacity, std::size_t& size) {
    return serialize_fixed_packet(
        output, capacity, size,
        [](std::size_t* const packet_size) { return MQTT_GetPingreqPacketSize(packet_size); },
        [](const MQTTFixedBuffer_t* const buffer) { return MQTT_SerializePingreq(buffer); });
}

/** @brief Decodes one complete PUBLISH and exposes views into its bytes. */
MqttCodecResult MqttCodec::decode_publish(
    const std::uint8_t* const packet, const std::size_t size, MqttPublishPacket& decoded) {
    decoded = {};
    MQTTPacketInfo_t info{};
    const auto packet_result = packet_info(packet, size, info);
    if (packet_result != MqttCodecResult::success) {
        return packet_result;
    }
    if ((info.type & 0xf0U) != MQTT_PACKET_TYPE_PUBLISH) {
        return MqttCodecResult::wrong_packet_type;
    }

    std::uint16_t packet_id = 0U;
    MQTTPublishInfo_t publish{};
    const auto result = map_status(MQTT_DeserializePublish(&info, &packet_id, &publish));
    if (result != MqttCodecResult::success) {
        return result;
    }
    decoded.topic = {publish.pTopicName, publish.topicNameLength};
    decoded.payload = static_cast<const std::uint8_t*>(publish.pPayload);
    decoded.payload_size = publish.payloadLength;
    decoded.packet_id = packet_id;
    decoded.qos = static_cast<std::uint8_t>(publish.qos);
    decoded.retained = publish.retain;
    decoded.duplicate = publish.dup;
    return MqttCodecResult::success;
}

/** @brief Decodes one MQTT CONNACK and preserves its return code. */
MqttCodecResult MqttCodec::decode_connack(const std::uint8_t* const packet, const std::size_t size,
    bool& session_present, std::uint8_t& return_code) {
    session_present = false;
    return_code = 0U;
    MQTTPacketInfo_t info{};
    const auto packet_result = packet_info(packet, size, info);
    if (packet_result != MqttCodecResult::success) {
        return packet_result;
    }
    if (info.type != MQTT_PACKET_TYPE_CONNACK) {
        return MqttCodecResult::wrong_packet_type;
    }
    if (info.remainingLength >= 2U) {
        return_code = info.pRemainingData[1];
    }
    return map_status(MQTT_DeserializeAck(&info, nullptr, &session_present));
}

/** @brief Decodes one MQTT SUBACK and preserves its packet identifier. */
MqttCodecResult MqttCodec::decode_suback(
    const std::uint8_t* const packet, const std::size_t size, std::uint16_t& packet_id) {
    packet_id = 0U;
    MQTTPacketInfo_t info{};
    const auto packet_result = packet_info(packet, size, info);
    if (packet_result != MqttCodecResult::success) {
        return packet_result;
    }
    if (info.type != MQTT_PACKET_TYPE_SUBACK) {
        return MqttCodecResult::wrong_packet_type;
    }
    return map_status(MQTT_DeserializeAck(&info, &packet_id, nullptr));
}

/** @brief Decodes one bounded MQTT UNSUBACK. */
MqttCodecResult MqttCodec::decode_unsuback(
    const std::uint8_t* const packet, const std::size_t size, std::uint16_t& packet_id) {
    packet_id = 0U;
    MQTTPacketInfo_t info{};
    const auto packet_result = packet_info(packet, size, info);
    if (packet_result != MqttCodecResult::success) {
        return packet_result;
    }
    if (info.type != MQTT_PACKET_TYPE_UNSUBACK) {
        return MqttCodecResult::wrong_packet_type;
    }
    if (info.remainingLength != 2U)
        return MqttCodecResult::invalid;
    return map_status(MQTT_DeserializeAck(&info, &packet_id, nullptr));
}

/** @brief Validates one MQTT PINGRESP using coreMQTT's deserializer. */
MqttCodecResult MqttCodec::decode_pingresp(
    const std::uint8_t* const packet, const std::size_t size) {
    MQTTPacketInfo_t info{};
    const auto packet_result = packet_info(packet, size, info);
    if (packet_result != MqttCodecResult::success) {
        return packet_result;
    }
    if (info.type != MQTT_PACKET_TYPE_PINGRESP) {
        return MqttCodecResult::wrong_packet_type;
    }
    return map_status(MQTT_DeserializeAck(&info, nullptr, nullptr));
}

} // namespace newmota
