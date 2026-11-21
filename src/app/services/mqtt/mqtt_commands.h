/*
  mqtt_commands.h - bounded MQTT command routing for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "core/command/command.h"
#include "core/message/message.h"
#include "newmota_features.h"
#include "services/mqtt/mqtt_codec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace newmota {

class MqttService;

/** @brief Exposes bounded MQTT command-routing outcomes. */
struct MqttCommandStats final {
    std::uint32_t received{};
    std::uint32_t ignored{};
    std::uint32_t invalid{};
    std::uint32_t chain_rejected{};
    std::uint32_t executed{};
    std::uint32_t response_queue_full{};
};

/**
 * @brief Converts MQTT command publications into the common command path.
 *
 * The MQTT callback only owns views into the current packet. This service
 * copies the command line into fixed storage, executes it synchronously in the
 * application context, and publishes the complete ordered command result as one
 * canonical response message. MqttService later routes those messages to
 * stat/.../RESULT.
 */
class MqttCommandService final {
  public:
    static constexpr std::size_t kCommandCapacity = MqttCodec::kMaximumPacketSize;

    /** @brief Binds MQTT command input to the common processor and router. */
    MqttCommandService(MessageRouter& messages, CommandProcessor& commands, MqttService& mqtt);

    /** @brief Adapter installed as MqttClient's inbound PUBLISH callback. */
    static void receive_publish(void* context, const MqttPublishPacket& packet);

    /** @brief Returns bounded command-routing counters. */
    [[nodiscard]] MqttCommandStats stats() const { return stats_; }

#if USE_TASMOTA_STATE
    /** @brief Publishes the next pending Status 0 section once the queue has room. */
    void poll();
#endif

  private:
    /** @brief Appends one command result fragment to bounded storage. */
    static bool append_response(void* context, std::string_view text);
    /** @brief Handles one inbound packet after the client validates its frame. */
    void handle_publish(const MqttPublishPacket& packet);
    /** @brief Copies one topic command and payload into a command line. */
    bool build_command(std::string_view command, const MqttPublishPacket& packet);
    /** @brief Publishes one JSON response into the canonical message router. */
    bool publish_response(std::string_view payload);
    /** @brief Runs the stored command line and publishes its complete response. */
    void execute_command();

    MessageRouter& messages_;
    CommandProcessor& commands_;
    MqttService& mqtt_;
    std::array<char, kCommandCapacity> command_{};
    std::size_t command_size_{};
    std::array<char, CanonicalMessage::kPayloadCapacity> response_{};
    std::size_t response_size_{};
    std::array<char, 32> response_suffix_{};
    bool response_fallback_{};
    MqttCommandStats stats_{};
};

} // namespace newmota
