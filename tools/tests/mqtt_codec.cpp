/*
  mqtt_codec.cpp - host tests for the bounded coreMQTT adapter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/mqtt/mqtt_codec.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace {

using newmota::MqttCodec;
using newmota::MqttCodecResult;

/** @brief Fails the test process with one concise diagnostic. */
bool expect(const bool condition, const char* const message) {
    if (!condition) {
        std::fprintf(stderr, "mqtt codec: %s\n", message);
    }
    return condition;
}

/** @brief Compares one byte range without relying on a sentinel terminator. */
bool equal_bytes(
    const std::uint8_t* const actual, const std::size_t size, const std::string_view expected) {
    return actual != nullptr && size == expected.size() &&
           std::memcmp(actual, expected.data(), size) == 0;
}

} // namespace

int main() {
    constexpr std::string_view topic = "tele/newmota/STATE";
    constexpr std::string_view payload = R"({"POWER":"ON"})";
    std::array<std::uint8_t, MqttCodec::kMaximumPacketSize> packet{};
    std::size_t packet_size = 0U;

    if (!expect(MqttCodec::serialize_publish(topic, payload, false, packet.data(), packet.size(),
                    packet_size) == MqttCodecResult::success,
            "QoS-0 publication did not serialize") ||
        !expect(packet[0] == 0x30U && packet[1] == 0x22U,
            "serialized fixed header is not MQTT 3.1.1 PUBLISH") ||
        !expect(packet_size == 36U, "serialized packet size changed") ||
        !expect(
            packet[2] == 0x00U && packet[3] == topic.size(), "serialized topic length is wrong")) {
        return 1;
    }

    newmota::MqttPublishPacket decoded{};
    if (!expect(MqttCodec::decode_publish(packet.data(), packet_size, decoded) ==
                    MqttCodecResult::success,
            "serialized publication did not decode") ||
        !expect(decoded.topic == topic, "decoded topic changed") ||
        !expect(equal_bytes(decoded.payload, decoded.payload_size, payload),
            "decoded payload changed") ||
        !expect(decoded.qos == 0U && !decoded.retained && !decoded.duplicate,
            "decoded QoS-0 flags changed") ||
        !expect(decoded.packet_id == 0U, "QoS-0 packet identifier is present")) {
        return 1;
    }

    if (!expect(MqttCodec::serialize_publish(topic, payload, true, packet.data(), packet.size(),
                    packet_size) == MqttCodecResult::success,
            "retained publication did not serialize") ||
        !expect(packet[0] == 0x31U, "retain flag was not serialized") ||
        !expect(MqttCodec::decode_publish(packet.data(), packet_size, decoded) ==
                    MqttCodecResult::success,
            "retained publication did not decode") ||
        !expect(decoded.retained, "retain flag was not decoded")) {
        return 1;
    }

    std::array<std::uint8_t, 4> small{};
    if (!expect(MqttCodec::serialize_publish(topic, payload, false, small.data(), small.size(),
                    packet_size) == MqttCodecResult::too_large,
            "small output buffer was accepted") ||
        !expect(MqttCodec::serialize_publish({}, payload, false, packet.data(), packet.size(),
                    packet_size) == MqttCodecResult::invalid,
            "empty topic was accepted")) {
        return 1;
    }

    std::array<std::uint8_t, MqttCodec::kMaximumPacketSize + 1U> oversized{};
    if (!expect(MqttCodec::decode_publish(oversized.data(), oversized.size(), decoded) ==
                    MqttCodecResult::too_large,
            "oversized packet was accepted")) {
        return 1;
    }

    constexpr std::string_view command_filter = "cmnd/newmota/#";
    if (!expect(MqttCodec::serialize_subscribe(command_filter, 7U, packet.data(), packet.size(),
                    packet_size) == MqttCodecResult::success,
            "command subscription did not serialize") ||
        !expect(packet[0] == 0x82U && packet[2] == 0x00U && packet[3] == 0x07U,
            "serialized SUBSCRIBE header is wrong") ||
        !expect(packet[packet_size - 1U] == 0x00U, "command subscription QoS is not zero")) {
        return 1;
    }

    std::uint16_t packet_id = 0U;
    const std::array<std::uint8_t, 5> accepted_suback{0x90U, 0x03U, 0x00U, 0x07U, 0x00U};
    if (!expect(MqttCodec::decode_suback(accepted_suback.data(), accepted_suback.size(),
                    packet_id) == MqttCodecResult::success &&
                    packet_id == 7U,
            "accepted SUBACK did not decode") ||
        !expect(MqttCodec::decode_suback(
                    std::array<std::uint8_t, 5>{0x90U, 0x03U, 0x00U, 0x07U, 0x80U}.data(), 5U,
                    packet_id) == MqttCodecResult::refused,
            "refused SUBACK was not reported")) {
        return 1;
    }

    if (!expect(MqttCodec::serialize_unsubscribe(command_filter, 9U, packet.data(), packet.size(),
                    packet_size) == MqttCodecResult::success &&
                    packet[0] == 0xa2U && packet[3] == 9U &&
                    packet_size == command_filter.size() + 6U,
            "UNSUBSCRIBE contains exact filter without QoS byte") ||
        !expect(MqttCodec::serialize_unsubscribe(command_filter, 0U, packet.data(), packet.size(),
                    packet_size) == MqttCodecResult::invalid,
            "zero unsubscribe id rejected") ||
        !expect(MqttCodec::serialize_unsubscribe(command_filter, 9U, packet.data(), 2U,
                    packet_size) == MqttCodecResult::too_large,
            "small unsubscribe buffer rejected"))
        return 1;
    const std::uint8_t unsuback[]{0xb0U, 2U, 0U, 9U};
    const std::uint8_t malformed_unsuback[]{0xb0U, 3U, 0U, 9U, 0U};
    if (!expect(MqttCodec::decode_unsuback(unsuback, sizeof unsuback, packet_id) ==
                        MqttCodecResult::success &&
                    packet_id == 9U,
            "UNSUBACK id decoded") ||
        !expect(MqttCodec::decode_unsuback(malformed_unsuback, sizeof malformed_unsuback,
                    packet_id) == MqttCodecResult::invalid,
            "extra UNSUBACK data rejected") ||
        !expect(MqttCodec::decode_unsuback(accepted_suback.data(), accepted_suback.size(),
                    packet_id) == MqttCodecResult::wrong_packet_type,
            "SUBACK cannot acknowledge unsubscribe"))
        return 1;

    newmota::MqttConnectParameters parameters{
        "client", {}, {}, 60U, true, "tele/lamp/LWT", "Offline", true};
    if (!expect(MqttCodec::serialize_connect(parameters, packet.data(), packet.size(),
                    packet_size) == MqttCodecResult::success &&
                    packet[0] == 0x10U && packet[9] == 0x26U,
            "CONNECT includes clean session and retained QoS-0 will") ||
        !expect(
            std::string_view{reinterpret_cast<const char*>(packet.data()), packet_size}.find(
                "tele/lamp/LWT") != std::string_view::npos &&
                std::string_view{reinterpret_cast<const char*>(packet.data()), packet_size}.find(
                    "Offline") != std::string_view::npos,
            "CONNECT contains exact will topic and Offline payload"))
        return 1;
    parameters.will_retained = false;
    if (!expect(MqttCodec::serialize_connect(parameters, packet.data(), packet.size(),
                    packet_size) == MqttCodecResult::success &&
                    packet[9] == 0x06U,
            "global no-retain clears will RETAIN without dropping will"))
        return 1;
    parameters.will_topic = "tele/+/LWT";
    if (!expect(MqttCodec::serialize_connect(parameters, packet.data(), packet.size(),
                    packet_size) == MqttCodecResult::invalid,
            "wildcard will topic rejected"))
        return 1;

    std::printf("mqtt codec tests passed\n");
    return 0;
}
