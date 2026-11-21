/*
  ping_service.h - Bounded asynchronous Ping command owner

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#pragma once

#include "core/command/command.h"

namespace newmota {
/** @brief Owns one campaign and retains its final result until both surfaces accept it. */
class PingService final {
  public:
    /** @brief Starts one asynchronous Ping command campaign. */
    [[nodiscard]] CommandStatus command(const CommandRequest&, CommandOutput&);
    /** @brief Advances DNS, probes, timeouts, and delivery for one campaign. */
    void poll(std::uint32_t now, bool network_ready, bool restart_pending);
    /** @brief Retries final-result delivery to each available output surface. */
    void deliver(CommandOutput::Writer console, void* console_context,
        CommandOutput::Writer message, void* message_context);

    /** @brief Reports whether a campaign or result delivery remains active. */
    [[nodiscard]] bool active() const { return state_ != State::idle; }

  private:
    enum class State : std::uint8_t { idle, resolving, probing, interval, draining, delivering };
    [[nodiscard]] const char* state_name() const;
    void finish(const char* error);
    [[nodiscard]] bool serialize_result(CommandOutput& output) const;
    std::array<char, 128> host_{};
    std::array<char, 16> address_{};
    std::array<char, 512> result_{};
    std::size_t result_size_{};
    const char* error_{""};
    std::uint32_t started_at_{}, sent_at_{}, token_{}, minimum_{}, maximum_{}, total_{};
    std::uint8_t limit_{}, sent_{}, success_{}, timeout_{};
    State state_{State::idle};
    bool fast_{}, sending_{}, console_pending_{}, message_pending_{}, has_result_{};
};
} // namespace newmota
