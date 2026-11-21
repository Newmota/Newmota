/*
  mqtt_wire.cpp - bounded MQTT framing and partial-write support for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/mqtt/mqtt_wire.h"

#if __has_include("newmota_features.h")
#include "newmota_features.h"
#endif
// Below, USE_BERRY decides: scripts can subscribe to anything, so a large publication must not end
// their session. Builds without Berry subscribe only to their own commands, and Safeboot has no
// room.

#include <algorithm>

#include <cstring>

namespace newmota {

/** @brief Validates MQTT 3.1.1 fixed-header flags for one packet type. */
bool MqttFrameReader::valid_flags(const std::uint8_t header) {
    const auto type = static_cast<std::uint8_t>(header >> 4U);
    const auto flags = static_cast<std::uint8_t>(header & 0x0fU);
    if (type == 3U) {
        const auto qos = static_cast<std::uint8_t>((flags >> 1U) & 0x03U);
        return qos != 3U && (qos != 0U || (flags & 0x08U) == 0U);
    }
    switch (type) {
    case 1U:
        return flags == 1U;
    case 2U:
        return flags == 0U;
    case 4U:
        return flags == 0x02U;
    case 5U:
        return flags == 0U;
    case 6U:
        return flags == 0x02U;
    case 7U:
        return flags == 0U;
    case 8U:
        return flags == 0x02U;
    case 9U:
        return flags == 0U;
    case 10U:
        return flags == 0x02U;
    case 11U:
        return flags == 0U;
    case 12U:
        return flags == 1U;
    case 13U:
        return flags == 0U;
    default:
        return false;
    }
}

#if !defined(USE_BERRY) || USE_BERRY
constexpr std::uint8_t kPacketPublish = 3U;
#endif

/** @brief Feeds partial bytes into the bounded remaining-length parser. */
MqttFrameResult MqttFrameReader::feed(
    const std::uint8_t* input, const std::size_t size, std::size_t& consumed) {
    consumed = 0U;
    if ((input == nullptr && size != 0U) || complete_) {
        return complete_ ? MqttFrameResult::complete : MqttFrameResult::invalid;
    }
    while (consumed < size) {
#if !defined(USE_BERRY) || USE_BERRY
        if (skipping_ != 0U) {
            const std::size_t count = std::min(skipping_, size - consumed);
            consumed += count;
            skipping_ -= count;
            if (skipping_ == 0U) {
                reset();
                return MqttFrameResult::skipped;
            }
            continue;
        }
#endif
        const auto byte = input[consumed++];
        if (frame_size_ == 0U) {
            if (!valid_flags(byte))
                return MqttFrameResult::invalid;
            frame_[frame_size_++] = byte;
            continue;
        }
        if (!header_ready_) {
            if (remaining_bytes_ >= 4U)
                return MqttFrameResult::invalid;
            if (frame_size_ >= frame_.size())
                return MqttFrameResult::too_large;
            frame_[frame_size_++] = byte;
            const auto value = static_cast<std::uint32_t>(byte & 0x7fU);
            constexpr std::uint32_t kMaximumRemainingLength = 0x0fffffffU;
            const auto contribution = value * remaining_multiplier_;
            if (remaining_length_ > (kMaximumRemainingLength - contribution)) {
                return MqttFrameResult::invalid;
            }
            remaining_length_ += contribution;
            ++remaining_bytes_;
            if ((byte & 0x80U) != 0U) {
                if (remaining_bytes_ == 4U)
                    return MqttFrameResult::invalid;
                remaining_multiplier_ *= 128U;
                continue;
            }
            if (remaining_bytes_ > 1U && value == 0U) {
                return MqttFrameResult::invalid;
            }
            header_ready_ = true;
            header_size_ = frame_size_;
            if (remaining_length_ > frame_.size() - header_size_) {
#if !defined(USE_BERRY) || USE_BERRY
                // One large publication must not end the session for everything else.
                if (packet_type() == kPacketPublish) {
                    skipping_ = remaining_length_;
                    continue;
                }
#endif
                return MqttFrameResult::too_large;
            }
            if (remaining_length_ == 0U) {
                complete_ = true;
                return MqttFrameResult::complete;
            }
            continue;
        }
        if (frame_size_ >= frame_.size())
            return MqttFrameResult::too_large;
        frame_[frame_size_++] = byte;
        if (frame_size_ == header_size_ + remaining_length_) {
            complete_ = true;
            return MqttFrameResult::complete;
        }
    }
    return MqttFrameResult::accepted;
}

/** @brief Clears one partial or complete frame before the next packet. */
void MqttFrameReader::reset() {
    frame_.fill(0U);
    frame_size_ = 0U;
    remaining_length_ = 0U;
    header_size_ = 0U;
    remaining_multiplier_ = 1U;
    remaining_bytes_ = 0U;
    header_ready_ = false;
    complete_ = false;
    skipping_ = 0U;
}

/** @brief Copies one complete outbound packet into the bounded writer. */
bool MqttPacketWriter::queue(const std::uint8_t* packet, const std::size_t size) {
    if (packet == nullptr || size == 0U || size > packet_.size() || pending()) {
        return false;
    }
    std::memcpy(packet_.data(), packet, size);
    size_ = size;
    offset_ = 0U;
    return true;
}

/** @brief Performs one partial nonblocking stream write. */
MqttWriteProgress MqttPacketWriter::poll(const platform::TcpStream& stream) {
    if (!pending())
        return MqttWriteProgress::idle;
    if (stream.state == nullptr || stream.write == nullptr) {
        return MqttWriteProgress::unavailable;
    }
    if (stream.state(stream.context) != platform::TcpStreamState::connected) {
        return MqttWriteProgress::not_connected;
    }
    const auto result = stream.write(stream.context, packet_.data() + offset_, size_ - offset_);
    if (result.size > size_ - offset_) {
        clear();
        return MqttWriteProgress::failed;
    }
    offset_ += result.size;
    if (offset_ == size_) {
        clear();
        return MqttWriteProgress::sent;
    }
    switch (result.state) {
    case platform::TcpStreamIoState::would_block:
        return MqttWriteProgress::blocked;
    case platform::TcpStreamIoState::progressed:
        if (result.size != 0U)
            return MqttWriteProgress::pending;
        clear();
        return MqttWriteProgress::failed;
    case platform::TcpStreamIoState::closed:
    case platform::TcpStreamIoState::failed:
    case platform::TcpStreamIoState::unavailable:
        clear();
        return MqttWriteProgress::failed;
    }
    clear();
    return MqttWriteProgress::failed;
}

/** @brief Clears one pending packet so a new connection can start cleanly. */
void MqttPacketWriter::clear() {
    packet_.fill(0U);
    size_ = 0U;
    offset_ = 0U;
}

} // namespace newmota
