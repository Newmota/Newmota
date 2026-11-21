/*
  mqtt_wire.h - bounded MQTT framing and partial-write support for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "newmota/platform/tcp_stream.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace newmota {

/** @brief Reports the result of feeding bytes into one bounded MQTT frame. */
enum class MqttFrameResult : std::uint8_t {
    accepted,
    complete,
    invalid,
    too_large,
    skipped,
};

/**
 * @brief Reassembles one MQTT 3.1.1 frame from partial stream reads.
 *
 * The reader validates the fixed-header flags and remaining-length encoding,
 * but deliberately leaves packet-specific semantic decoding to the selected
 * MQTT codec. A caller must copy or decode the complete frame before reset.
 */
class MqttFrameReader final {
  public:
    // This covers the first 1024-byte canonical JSON payload plus MQTT topic
    // and packet fields while keeping the stream boundary statically bounded.
    static constexpr std::size_t kMaximumPacketSize = 1280;

    /** @brief Constructs an empty frame reader. */
    constexpr MqttFrameReader() = default;

    /**
     * @brief Feeds as many bytes as fit before one complete frame is ready.
     * @param input Source bytes; may be null only when @p size is zero.
     * @param size Number of source bytes offered by the stream.
     * @param consumed Number of source bytes consumed by this call.
     * @return The framing result; complete stops before any following frame.
     */
    [[nodiscard]] MqttFrameResult feed(
        const std::uint8_t* input, std::size_t size, std::size_t& consumed);

    /** @brief Returns the complete frame bytes, or partial bytes while reading. */
    [[nodiscard]] const std::uint8_t* data() const { return frame_.data(); }

    /** @brief Returns the number of bytes currently stored in the frame. */
    [[nodiscard]] std::size_t size() const { return frame_size_; }

    /** @brief Returns the MQTT packet type nibble, or zero before a header. */
    [[nodiscard]] std::uint8_t packet_type() const {
        return frame_size_ == 0U ? 0U : static_cast<std::uint8_t>(frame_[0] >> 4U);
    }

    /** @brief Reports whether one complete frame is ready for semantic decode. */
    [[nodiscard]] bool complete() const { return complete_; }

    /** @brief Discards the current frame and all partial parser state. */
    void reset();

  private:
    /** @brief Validates the packet-type-specific fixed-header flags. */
    [[nodiscard]] static bool valid_flags(std::uint8_t header);

    std::array<std::uint8_t, kMaximumPacketSize> frame_{};
    std::size_t frame_size_{};
    std::size_t remaining_length_{};
    std::size_t header_size_{};
    std::uint32_t remaining_multiplier_{1U};
    std::uint8_t remaining_bytes_{};
    bool header_ready_{};
    bool complete_{};
    std::size_t skipping_{};
};

/** @brief Reports one bounded packet-writer poll. */
enum class MqttWriteProgress : std::uint8_t {
    idle,
    blocked,
    pending,
    sent,
    not_connected,
    failed,
    unavailable,
};

/**
 * @brief Owns one outbound MQTT frame while a stream performs partial writes.
 *
 * Only one frame is queued at a time. A disconnected stream preserves the
 * queued bytes so the protocol owner can decide whether reconnect replay is
 * valid; a hard I/O failure clears the frame because its delivery boundary is
 * no longer known.
 */
class MqttPacketWriter final {
  public:
    static constexpr std::size_t kPacketCapacity = MqttFrameReader::kMaximumPacketSize;

    /** @brief Queues one complete packet by copying it into owned storage. */
    [[nodiscard]] bool queue(const std::uint8_t* packet, std::size_t size);
    /** @brief Attempts one nonblocking partial write to the native stream. */
    [[nodiscard]] MqttWriteProgress poll(const platform::TcpStream& stream);

    /** @brief Reports whether a packet remains partially written. */
    [[nodiscard]] bool pending() const { return size_ != 0U; }

    /** @brief Returns bytes still waiting for the native stream. */
    [[nodiscard]] std::size_t remaining() const { return size_ - offset_; }

    /** @brief Drops the queued frame and clears all packet bytes. */
    void clear();

  private:
    std::array<std::uint8_t, kPacketCapacity> packet_{};
    std::size_t size_{};
    std::size_t offset_{};
};

} // namespace newmota
