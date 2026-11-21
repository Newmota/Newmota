/*
  audio_network.h - Native audio HTTP source and microphone listener

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_network.h
 * @brief Bounded HTTP/ICY playback and compressed port-81 microphone streaming.
 */
#pragma once
#include <string_view>

namespace newmota::audio {
/** @brief Binds the real network providers to independent native TCP contexts. */
void initialize_network();
/** @brief Advances listener requests and native session lifecycle without blocking. */
void poll_network();
/** @brief The network source's `icy-name` and latest ICY StreamTitle; empty without them.
 * The views stay valid until the next application-loop poll.
 */
std::string_view network_station();
std::string_view network_title();
} // namespace newmota::audio
