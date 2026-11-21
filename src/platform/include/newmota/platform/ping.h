/*
  ping.h - Platform asynchronous IPv4 ping transport

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#pragma once

#include "newmota/platform/network.h"

namespace newmota::platform {
enum class PingState : std::uint8_t { idle, resolving, ready, sending, waiting, replied, failed };

struct PingSnapshot final {
    PingState state{PingState::idle};
    char address[16]{};
    std::uint32_t sent_at{}, elapsed_ms{};
};

/** @brief Copies one IPv4/DNS destination; busy until cancellation and DNS have drained. */
[[nodiscard]] NetworkResult ping_start(std::string_view host);
/** @brief Queues one echo; native reply acceptance expires after one second. */
[[nodiscard]] NetworkResult ping_send(std::uint32_t token, std::uint16_t sequence);
/** @brief Copies stable native progress; never waits for the core, DNS, or a reply. */
[[nodiscard]] PingSnapshot ping_poll();
/** @brief Cancels on the core; poll until idle before reusing the fixed slot. */
void ping_cancel();
} // namespace newmota::platform
