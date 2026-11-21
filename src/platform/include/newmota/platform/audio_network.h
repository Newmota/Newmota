/*
  audio_network.h - Dedicated native audio TCP endpoints

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_network.h
 * @brief Audio owns independent client/listener TCP resources, separate from MQTT/OTA.
 */
#pragma once
#include "newmota/platform/tcp_stream.h"
#include <cstdint>

namespace newmota::platform {
enum class AudioListenerState : std::uint8_t { closed, starting, listening, failed };
/** @brief Returns the dedicated outgoing plaintext audio stream. */
TcpStream audio_tcp_stream();
/** @brief Returns the one accepted port-81 connection; it cannot initiate outbound TCP. */
TcpStream audio_listener_stream();
/** @brief Reports whether the accepted stream has no buffered or unacknowledged output. */
bool audio_listener_idle();
/** @brief Queues listener creation/cleanup on the native lwIP core without blocking. */
bool audio_listen(bool enabled);
/** @brief Reports actual listener creation outcome and maintains queued native cleanup. */
AudioListenerState audio_listener_state();
} // namespace newmota::platform
