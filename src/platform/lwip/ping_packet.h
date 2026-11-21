/*
  ping_packet.h - Bounded ICMP wire encoding and validation

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace newmota::platform::ping_packet {
inline constexpr std::size_t kEchoSize = 40; // Eight header bytes plus 32 payload bytes.
inline constexpr std::uint32_t kReplyTimeout = 1000;
using Echo = std::array<std::uint8_t, kEchoSize>;

inline std::uint16_t word(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((static_cast<unsigned int>(p[0]) << 8U) | p[1]);
}

inline std::uint16_t checksum(const std::uint8_t* bytes, std::size_t size) {
    std::uint32_t sum = 0;
    while (size >= 2) {
        sum += word(bytes);
        bytes += 2;
        size -= 2;
    }
    if (size != 0)
        sum += static_cast<std::uint32_t>(*bytes) << 8U;
    while (sum >> 16U)
        sum = (sum & 0xffffU) + (sum >> 16U);
    return static_cast<std::uint16_t>(~sum);
}

inline Echo request(std::uint32_t token, std::uint16_t sequence) {
    Echo packet{};
    packet[0] = 8;
    packet[4] = static_cast<std::uint8_t>(token >> 24U);
    packet[5] = static_cast<std::uint8_t>(token >> 16U);
    packet[6] = static_cast<std::uint8_t>(sequence >> 8U);
    packet[7] = static_cast<std::uint8_t>(sequence);
    for (std::size_t i = 8; i < packet.size(); ++i)
        packet[i] = static_cast<std::uint8_t>((token >> ((i % 4U) * 8U)) ^ i);
    const auto sum = checksum(packet.data(), packet.size());
    packet[2] = static_cast<std::uint8_t>(sum >> 8U);
    packet[3] = static_cast<std::uint8_t>(sum);
    return packet;
}

/** @brief Validates a fully copied IPv4 datagram including options and exact echo identity. */
inline bool reply(const std::uint8_t* bytes, std::size_t size, const Echo& expected) {
    if (size < 20 || (bytes[0] >> 4U) != 4)
        return false;
    const auto header = static_cast<std::size_t>(bytes[0] & 15U) * 4U;
    if (header < 20 || header > 60 || size != header + kEchoSize || word(bytes + 2) != size ||
        bytes[9] != 1 || (word(bytes + 6) & 0x3fffU) != 0)
        return false;
    const auto* echo = bytes + header;
    if (echo[0] != 0 || echo[1] != 0 || checksum(echo, kEchoSize) != 0)
        return false;
    for (std::size_t i = 4; i < kEchoSize; ++i)
        if (echo[i] != expected[i])
            return false;
    return true;
}
} // namespace newmota::platform::ping_packet
