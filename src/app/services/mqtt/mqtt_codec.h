/*
  mqtt_codec.h - Newmota's bounded coreMQTT serializer adapter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "services/mqtt/mqtt_wire.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace newmota {

/** @brief Reports the result of one bounded MQTT semantic codec operation. */
enum class MqttCodecResult : std::uint8_t {
    success,
    invalid,
    too_large,
    wrong_packet_type,
    refused,
};

/**
 * @brief Describes an incoming PUBLISH without copying its topic or payload.
 *
 * The views refer to the complete packet supplied to
 * @ref MqttCodec::decode_publish and remain valid only while that packet's
 * storage remains unchanged.  The codec does not retain the packet pointer.
 */
struct MqttPublishPacket final {
    std::string_view topic{};
    const std::uint8_t* payload{};
    std::size_t payload_size{};
    std::uint16_t packet_id{};
    std::uint8_t qos{};
    bool retained{};
    bool duplicate{};
};

/** @brief Describes the bounded MQTT 3.1.1 CONNECT fields to serialize. */
struct MqttConnectParameters final {
    std::string_view client_id{};
    std::string_view username{};
    std::string_view password{};
    std::uint16_t keep_alive_seconds{60U};
    bool clean_session{true};
    std::string_view will_topic{};
    std::string_view will_payload{};
    bool will_retained{true};
};

/**
 * @brief Adapts the pinned coreMQTT serializer to Newmota's bounded wire API.
 *
 * Only the low-level MQTT 3.1.1 serializer/deserializer layer is used here.
 * It owns no connection, retry, authentication, TLS, or message-routing
 * policy.  Newmota validates the complete frame and its 1280-byte limit before
 * asking coreMQTT to decode packet semantics.
 */
class MqttCodec final {
  public:
    static constexpr std::size_t kMaximumPacketSize = MqttFrameReader::kMaximumPacketSize;

    /**
     * @brief Serializes one bounded QoS-0 PUBLISH packet.
     * @param topic MQTT topic name; it must fit the MQTT UTF-8 length field.
     * @param payload Complete payload bytes, including canonical JSON.
     * @param retained Whether the broker should retain the publication.
     * @param output Destination packet buffer owned by the caller.
     * @param capacity Available bytes in @p output.
     * @param size Receives the exact serialized packet size on success.
     * @return The codec result; no partial packet is reported as successful.
     */
    [[nodiscard]] static MqttCodecResult serialize_publish(std::string_view topic,
        std::string_view payload, bool retained, std::uint8_t* output, std::size_t capacity,
        std::size_t& size);

    /**
     * @brief Serializes one bounded QoS-0 MQTT SUBSCRIBE packet.
     * @param topic_filter Topic filter accepted by the broker.
     * @param packet_id Nonzero packet identifier matched by SUBACK.
     * @param output Destination packet buffer owned by the caller.
     * @param capacity Available bytes in @p output.
     * @param size Receives the exact serialized packet size on success.
     * @return The codec result; no partial packet is reported as successful.
     */
    [[nodiscard]] static MqttCodecResult serialize_subscribe(std::string_view topic_filter,
        std::uint16_t packet_id, std::uint8_t* output, std::size_t capacity, std::size_t& size);

    /** @brief Serializes one bounded UNSUBSCRIBE matched by UNSUBACK. */
    [[nodiscard]] static MqttCodecResult serialize_unsubscribe(std::string_view topic_filter,
        std::uint16_t packet_id, std::uint8_t* output, std::size_t capacity, std::size_t& size);
    /** @brief Decodes one UNSUBACK and its nonzero packet identifier. */
    [[nodiscard]] static MqttCodecResult decode_unsuback(
        const std::uint8_t* packet, std::size_t size, std::uint16_t& packet_id);

    /**
     * @brief Serializes one MQTT 3.1.1 CONNECT packet without a last will.
     * @param parameters Bounded client identity and optional credentials.
     * @param output Destination packet buffer owned by the caller.
     * @param capacity Available bytes in @p output.
     * @param size Receives the exact serialized packet size on success.
     * @return The codec result; no partial packet is reported as successful.
     */
    [[nodiscard]] static MqttCodecResult serialize_connect(const MqttConnectParameters& parameters,
        std::uint8_t* output, std::size_t capacity, std::size_t& size);

    /**
     * @brief Serializes one MQTT PINGREQ packet.
     * @param output Destination packet buffer owned by the caller.
     * @param capacity Available bytes in @p output.
     * @param size Receives the exact serialized packet size on success.
     * @return The codec result; no partial packet is reported as successful.
     */
    [[nodiscard]] static MqttCodecResult serialize_pingreq(
        std::uint8_t* output, std::size_t capacity, std::size_t& size);

    /**
     * @brief Decodes one complete incoming PUBLISH packet in place.
     * @param packet Exact MQTT packet bytes, without trailing bytes.
     * @param size Number of bytes in @p packet.
     * @param decoded Receives views into @p packet on success.
     * @return The codec result; the decoded views are invalid after packet data
     *         is released or modified.
     */
    [[nodiscard]] static MqttCodecResult decode_publish(
        const std::uint8_t* packet, std::size_t size, MqttPublishPacket& decoded);

    /**
     * @brief Decodes one CONNACK and returns its MQTT return code.
     * @param packet Exact MQTT packet bytes, without trailing bytes.
     * @param size Number of bytes in @p packet.
     * @param session_present Receives the broker session-present flag.
     * @param return_code Receives the MQTT 3.1.1 connect return code.
     * @return Refused is returned for a valid nonzero broker return code.
     */
    [[nodiscard]] static MqttCodecResult decode_connack(const std::uint8_t* packet,
        std::size_t size, bool& session_present, std::uint8_t& return_code);

    /**
     * @brief Decodes one SUBACK and returns its packet identifier.
     * @param packet Exact MQTT packet bytes, without trailing bytes.
     * @param size Number of bytes in @p packet.
     * @param packet_id Receives the SUBACK packet identifier.
     * @return Refused when the broker rejected the subscription.
     */
    [[nodiscard]] static MqttCodecResult decode_suback(
        const std::uint8_t* packet, std::size_t size, std::uint16_t& packet_id);

    /**
     * @brief Validates one PINGRESP packet.
     * @param packet Exact MQTT packet bytes, without trailing bytes.
     * @param size Number of bytes in @p packet.
     * @return Success for a valid MQTT PINGRESP.
     */
    [[nodiscard]] static MqttCodecResult decode_pingresp(
        const std::uint8_t* packet, std::size_t size);
};

} // namespace newmota
