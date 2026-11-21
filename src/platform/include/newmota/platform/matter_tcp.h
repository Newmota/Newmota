/*
  matter_tcp.h - IPv6 TCP connections for Matter large messages

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "newmota/platform/matter_udp.h"

#include <cstddef>

namespace newmota::platform {

/** @brief Largest message without its length prefix; Berry bytes stop at 32 KiB. */
inline constexpr std::size_t kMatterTcpMaxMessage = 32000;

/** @brief Opens the listener on the Matter port; call repeatedly, it retries until bound. */
void matter_tcp_poll();
/** @brief Closes the listener and every connection, and drops queued messages. */
void matter_tcp_close();
/**
 * @brief Takes the oldest complete message, or nullptr.
 * @param connection Receives the handle of the connection it arrived on.
 */
[[nodiscard]] UdpDatagram* matter_tcp_receive(int& connection);
/** @brief Queues one message, which gets its length prefix, for a connection without waiting. */
[[nodiscard]] bool matter_tcp_send(int connection, const void* data, std::size_t size);
/** @brief Starts a connection to a peer. @return Its handle, negative without a free slot. */
[[nodiscard]] int matter_tcp_connect(const UdpPeer& peer);
/** @brief Reports whether a handle still names a connection that is opening or open. */
[[nodiscard]] bool matter_tcp_alive(int connection);

} // namespace newmota::platform
