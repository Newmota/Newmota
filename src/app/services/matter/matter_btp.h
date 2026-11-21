/*
  matter_btp.h - bounded Bluetooth Transport Protocol for Matter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "services/matter/matter_wire.h"

#include <cstddef>
#include <cstdint>

namespace newmota::matter {

/**
 * @brief One BTP connection: handshake, segmentation and windows, without any Bluetooth calls.
 * Writes from the peer are untrusted; a violation ends the connection instead of being repaired.
 */
class Btp final {
  public:
    enum class Received : std::uint8_t { nothing, message, violation };

    /** @brief Forgets everything, keeping the buffers. */
    void reset();
    /** @brief Handles one characteristic write; att_mtu bounds the segments we send. */
    [[nodiscard]] Received on_write(
        const std::uint8_t* data, std::size_t size, std::uint16_t att_mtu);

    /** @brief The message after Received::message, valid until the next on_write. */
    [[nodiscard]] const std::uint8_t* message() const { return receive_; }

    [[nodiscard]] std::size_t message_size() const { return received_; }

    /** @brief Records that the peer subscribed, which permits the handshake response. */
    void on_subscribed() { subscribed_ = true; }

    /** @brief Takes a copy of one outgoing message. @return False when the queue is full. */
    [[nodiscard]] bool enqueue(const std::uint8_t* data, std::size_t size);
    /**
     * @brief Builds the next indication while none is in flight and the window allows it.
     * @return Its size, or 0 when there is nothing to send now.
     */
    [[nodiscard]] std::size_t next(std::uint8_t* output, std::size_t capacity);

    /** @brief Records that the peer confirmed the indication in flight. */
    void on_confirmed() { in_flight_ = false; }

    [[nodiscard]] bool established() const { return established_; }

    static constexpr std::uint8_t kWindow = 4; // Some platforms queue only four write events.
    static constexpr std::size_t kMaxSegment = 244;
    static constexpr std::size_t kQueue = 4;

    Btp() = default;
    Btp(const Btp&) = delete;
    Btp& operator=(const Btp&) = delete;
    ~Btp();

  private:
    void release_outgoing();

    std::uint8_t receive_[kMaxFrameSize]{};
    std::size_t received_{};
    std::size_t expected_{};
    bool assembling_{};
    std::uint8_t* outgoing_[kQueue]{};
    std::uint16_t outgoing_size_[kQueue]{};
    std::size_t outgoing_offset_{};
    std::size_t segment_{};
    std::uint8_t peer_window_{};
    std::uint8_t next_receive_{};
    std::uint8_t last_sent_{};
    std::uint8_t last_acknowledged_{};
    std::uint8_t ack_value_{};
    std::uint8_t bare_outstanding_{};
    bool ack_unsent_{};
    bool ack_required_{};
    bool handshake_pending_{};
    bool subscribed_{};
    bool established_{};
    bool in_flight_{};
};

} // namespace newmota::matter
