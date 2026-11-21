/*
  matter_udp.h - IPv6 UDP sockets for Matter and DNS-SD

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

namespace newmota::platform {

inline constexpr std::size_t kMatterAddressCount = 3;

/** @brief An IPv6 peer with the scope that link-local replies need. */
struct UdpPeer {
    std::array<std::uint8_t, 16> address{};
    std::uint16_t port{};
    std::uint8_t zone{};
};

/** @brief One received datagram, owned by the caller until matter_udp_release. */
struct UdpDatagram {
    UdpPeer peer{};
    bool mdns{};
    std::uint16_t size{};
    std::uint8_t data[1]{};
};

/** @brief Requests both sockets; call repeatedly, it also follows link and address changes. */
void matter_udp_poll();
/** @brief Reports whether a link has a usable IPv6 address and both sockets are bound. */
[[nodiscard]] bool matter_udp_ready();
/** @brief Closes both sockets and drops queued datagrams. */
void matter_udp_close();
/** @brief Takes the oldest datagram, or nullptr. Bounded in count and bytes on the network side. */
[[nodiscard]] UdpDatagram* matter_udp_receive();
/** @brief Frees a datagram from matter_udp_receive. */
void matter_udp_release(UdpDatagram* datagram);
/** @brief Queues a copy for sending to a peer without waiting. */
[[nodiscard]] bool matter_udp_send(const UdpPeer& peer, const void* data, std::size_t size);
/** @brief Queues a copy for the DNS-SD multicast group or, with a peer, for one legacy client. */
[[nodiscard]] bool matter_mdns_send(const UdpPeer* peer, const void* data, std::size_t size);
/**
 * @brief Copies the valid IPv6 addresses of the link.
 * @return The count, at most kMatterAddressCount. It changes with the generation below.
 */
[[nodiscard]] std::size_t matter_ipv6_addresses(
    std::array<std::array<std::uint8_t, 16>, kMatterAddressCount>& addresses,
    std::uint32_t& generation);

} // namespace newmota::platform
