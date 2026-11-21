/*
  matter_btp.cpp - bounded Bluetooth Transport Protocol for Matter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/matter/matter_btp.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace newmota::matter {
namespace {
constexpr std::uint8_t kFlagBegin = 0x01;
constexpr std::uint8_t kFlagContinue = 0x02;
constexpr std::uint8_t kFlagEnd = 0x04;
constexpr std::uint8_t kFlagAck = 0x08;
constexpr std::uint8_t kFlagManagement = 0x20;
constexpr std::uint8_t kFlagHandshake = 0x40;
constexpr std::uint8_t kHandshakeFlags = 0x65;
constexpr std::uint8_t kHandshakeOpcode = 0x6C;
constexpr std::uint8_t kVersion = 4;
constexpr std::size_t kHandshakeRequest = 9;
constexpr std::size_t kMinSegment = 20;
constexpr std::size_t kAttOverhead = 3;
constexpr std::size_t kSegmentHeader = 5; // Flags, acknowledgement, sequence, message length.
} // namespace

Btp::~Btp() {
    release_outgoing();
}

void Btp::release_outgoing() {
    for (auto*& message : outgoing_) {
        std::free(message);
        message = nullptr;
    }
    std::fill(std::begin(outgoing_size_), std::end(outgoing_size_), std::uint16_t{0});
}

void Btp::reset() {
    release_outgoing();
    received_ = expected_ = outgoing_offset_ = segment_ = 0;
    assembling_ = ack_unsent_ = ack_required_ = handshake_pending_ = subscribed_ = established_ =
        in_flight_ = false;
    peer_window_ = next_receive_ = last_sent_ = last_acknowledged_ = ack_value_ =
        bare_outstanding_ = 0;
}

Btp::Received Btp::on_write(
    const std::uint8_t* const data, const std::size_t size, const std::uint16_t att_mtu) {
    if (size == 0U)
        return Received::violation;
    const std::uint8_t flags = data[0];
    if (flags == kHandshakeFlags) {
        if (established_ || handshake_pending_ || size != kHandshakeRequest ||
            data[1] != kHandshakeOpcode)
            return Received::violation;
        bool supported = false;
        for (std::size_t index = 2; index < 6U; ++index) {
            supported =
                supported || (data[index] & 0x0FU) == kVersion || (data[index] >> 4U) == kVersion;
        }
        if (!supported || data[8] == 0U)
            return Received::violation;
        const std::size_t requested = data[6] | (std::size_t{data[7]} << 8U);
        std::size_t available = att_mtu > kAttOverhead ? att_mtu - kAttOverhead : kMinSegment;
        if (requested != 0U)
            available = std::min(available, requested);
        segment_ = std::clamp(available, kMinSegment, kMaxSegment);
        peer_window_ = std::min(data[8], kWindow);
        handshake_pending_ = true;
        return Received::nothing;
    }
    if (!established_ || (flags & (kFlagHandshake | kFlagManagement)) != 0U)
        return Received::violation;
    std::size_t offset = 1;
    if ((flags & kFlagAck) != 0U) {
        if (offset >= size)
            return Received::violation;
        // An acknowledgement may only cover what was sent and not yet acknowledged.
        const auto outstanding = static_cast<std::uint8_t>(last_sent_ - last_acknowledged_);
        const auto covered = static_cast<std::uint8_t>(data[offset] - last_acknowledged_);
        if (covered > outstanding)
            return Received::violation;
        last_acknowledged_ = data[offset];
        if (last_acknowledged_ == last_sent_)
            bare_outstanding_ = 0;
        ++offset;
    }
    if (offset >= size || data[offset] != next_receive_)
        return Received::violation;
    // A bare acknowledgement is not acknowledged by itself, or both ends would never fall idle.
    // Its number still travels on the next segment we send anyway.
    const bool bare = (flags & (kFlagBegin | kFlagContinue | kFlagEnd)) == 0U;
    ack_value_ = data[offset];
    ack_unsent_ = true;
    ack_required_ = ack_required_ || !bare;
    ++next_receive_;
    ++offset;
    if ((flags & kFlagBegin) != 0U) {
        if (assembling_ || (flags & kFlagContinue) != 0U || size - offset < 2U)
            return Received::violation;
        expected_ = data[offset] | (std::size_t{data[offset + 1U]} << 8U);
        offset += 2;
        if (expected_ > sizeof(receive_))
            return Received::violation;
        received_ = 0;
        assembling_ = true;
    } else if ((flags & kFlagContinue) != 0U) {
        if (!assembling_)
            return Received::violation;
    } else if (size != offset || (flags & kFlagEnd) != 0U) {
        return Received::violation; // Only a bare acknowledgement has no segment flags.
    } else {
        return Received::nothing;
    }
    const std::size_t count = size - offset;
    if (count > expected_ - received_)
        return Received::violation;
    std::memcpy(receive_ + received_, data + offset, count);
    received_ += count;
    if ((flags & kFlagEnd) == 0U)
        return Received::nothing;
    assembling_ = false;
    return received_ == expected_ ? Received::message : Received::violation;
}

bool Btp::enqueue(const std::uint8_t* const data, const std::size_t size) {
    if (size == 0U || size > kMaxFrameSize)
        return false;
    for (std::size_t index = 0; index < kQueue; ++index) {
        if (outgoing_[index] != nullptr)
            continue;
        auto* const copy = static_cast<std::uint8_t*>(std::malloc(size));
        if (copy == nullptr)
            return false;
        std::memcpy(copy, data, size);
        outgoing_[index] = copy;
        outgoing_size_[index] = static_cast<std::uint16_t>(size);
        return true;
    }
    return false;
}

std::size_t Btp::next(std::uint8_t* const output, const std::size_t capacity) {
    if (in_flight_ || capacity < kSegmentHeader + 1U)
        return 0;
    if (handshake_pending_) {
        if (!subscribed_ || capacity < 6U)
            return 0;
        output[0] = kHandshakeFlags;
        output[1] = kHandshakeOpcode;
        output[2] = kVersion;
        output[3] = static_cast<std::uint8_t>(segment_);
        output[4] = static_cast<std::uint8_t>(segment_ >> 8U);
        output[5] = kWindow;
        handshake_pending_ = false;
        established_ = true;
        in_flight_ = true;
        return 6; // Counts as sequence number zero.
    }
    if (!established_)
        return 0;
    // Bare acknowledgements do not count: a peer that never acknowledges them must not close the
    // window, and an acknowledgement may always go out, or both ends could wait for each other.
    const auto outstanding = static_cast<std::uint8_t>(last_sent_ - last_acknowledged_);
    const bool window_open =
        static_cast<std::uint8_t>(outstanding - std::min(outstanding, bare_outstanding_)) <
        peer_window_;
    const bool has_data = outgoing_[0] != nullptr && window_open;
    if (!has_data && !ack_required_)
        return 0;
    if (!has_data)
        ++bare_outstanding_;
    std::size_t offset = 1;
    std::uint8_t flags = 0;
    if (ack_unsent_) {
        flags |= kFlagAck;
        output[offset++] = ack_value_;
        ack_unsent_ = ack_required_ = false;
    }
    output[offset++] = ++last_sent_;
    if (has_data) {
        const std::size_t total = outgoing_size_[0];
        if (outgoing_offset_ == 0U) {
            flags |= kFlagBegin;
            output[offset++] = static_cast<std::uint8_t>(total);
            output[offset++] = static_cast<std::uint8_t>(total >> 8U);
        } else {
            flags |= kFlagContinue;
        }
        const std::size_t room = std::min(segment_, capacity) - offset;
        const std::size_t count = std::min(room, total - outgoing_offset_);
        std::memcpy(output + offset, outgoing_[0] + outgoing_offset_, count);
        offset += count;
        outgoing_offset_ += count;
        if (outgoing_offset_ == total) {
            flags |= kFlagEnd;
            std::free(outgoing_[0]);
            std::move(std::begin(outgoing_) + 1, std::end(outgoing_), std::begin(outgoing_));
            std::move(std::begin(outgoing_size_) + 1, std::end(outgoing_size_),
                std::begin(outgoing_size_));
            outgoing_[kQueue - 1U] = nullptr;
            outgoing_size_[kQueue - 1U] = 0;
            outgoing_offset_ = 0;
        }
    }
    output[0] = flags;
    in_flight_ = true;
    return offset;
}

} // namespace newmota::matter
