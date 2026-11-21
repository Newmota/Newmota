/*
  recovery_web_service.h - compact Safeboot browser recovery service

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
#include <string_view>

namespace newmota::platform {
struct WebExchange;
} // namespace newmota::platform

namespace newmota {

class Settings;
class UpdateWebService;

/** @brief Serves only the small authenticated Safeboot firmware recovery page. */
class RecoveryWebService final {
  public:
    /** @brief Binds persisted credentials and the native recovery writer. */
    RecoveryWebService(Settings& settings, UpdateWebService& updates);

    /** @brief Starts the recovery listener and processes one bounded exchange. */
    void poll(std::uint32_t now, bool network_ready);

  private:
    struct Session final {
        std::array<char, 33> token{}, csrf{};
        std::uint32_t last_used{}, rate_start{};
        unsigned int requests{};
    };

    /** @brief Clears browser sessions when the stored password changes. */
    void refresh_credential();
    /** @brief Finds the one unexpired Safeboot browser session. */
    Session* session(std::string_view cookie, std::uint32_t now);
    /** @brief Validates the request and handles recovery-only endpoints. */
    void handle(platform::WebExchange& exchange, std::uint32_t now);

    Settings& settings_;
    UpdateWebService& updates_;
    Session session_{};
    std::array<char, 64> credential_{};
    std::size_t credential_size_{};
    std::uint32_t login_window_{}, last_start_{};
    unsigned int login_attempts_{};
    bool started_{};
    bool attempted_start_{};
};

} // namespace newmota
