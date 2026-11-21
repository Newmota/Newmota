/*
  wget_service.h - bounded asynchronous HTTP file download

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#pragma once

#include "core/command/command.h"
#include "core/storage/filesystem.h"
#include "newmota/platform/tcp_stream.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

namespace newmota {

/** @brief Owns one HTTP download and commits its staging file only after completion. */
class WgetService final {
  public:
    using BusyProbe = bool (*)(void*);

    /** @brief Binds the native stream shared with MQTT and firmware URL updates. */
    explicit WgetService(platform::TcpStream stream) : stream_(stream) {}

    /** @brief Binds the application probe for existing transfer and restart leases. */
    void set_busy_probe(BusyProbe probe, void* context) {
        busy_probe_ = probe;
        busy_context_ = context;
    }

    enum class StartResult : std::uint8_t { started, busy, invalid_url, unavailable, no_memory };

    /** @brief Starts one URL download or reports the current and last transfer status. */
    [[nodiscard]] CommandStatus command(const CommandRequest& request, CommandOutput& output);
    /** @brief Starts one download into the root file named after the URL's basename. */
    [[nodiscard]] StartResult start(std::string_view url);
    /** @brief Whether the last download finished and its file was committed. */
    [[nodiscard]] bool completed() const;

    /** @brief The file of the current or last download. */
    [[nodiscard]] const char* destination() const { return destination_.data(); }

    /** @brief The error of the last failed download, or "". */
    [[nodiscard]] const char* error() const { return error_; }

    /** @brief Advances one bounded transport/storage step and expires stalled operations. */
    void poll(std::uint32_t now, bool network_ready, bool restart_pending);

    /** @brief Includes preparation and native connection cleanup in the transfer lease. */
    [[nodiscard]] bool active() const { return state_ != State::idle; }

    /** @brief Allows MQTT response delivery during preparation, then reserves its stream. */
    [[nodiscard]] bool owns_stream() const { return active() && state_ != State::preparing; }

  private:
    enum class State : std::uint8_t {
        idle,
        preparing,
        closing,
        connecting,
        sending,
        headers,
        body,
        checking_end,
        draining
    };
    /** @brief Reports the live phase, or the retained outcome after cleanup. */
    [[nodiscard]] const char* state_name() const;
    /** @brief Parses bounded plaintext URLs and derives a root-level destination basename. */
    [[nodiscard]] bool load_url(std::string_view url);
    /** @brief Accepts a direct length-delimited HTTP 200 response. */
    [[nodiscard]] bool parse_headers();
    /** @brief Writes a bounded body slice and commits only after the complete body. */
    [[nodiscard]] bool consume(const std::uint8_t* bytes, std::size_t size);
    /** @brief Synchronizes staging before atomically replacing the destination. */
    void finish();
    /** @brief Retains an error and discards staging before draining the stream. */
    void fail(const char* reason);
    /** @brief Releases storage and starts bounded asynchronous connection cleanup. */
    void release();
    platform::TcpStream stream_{};
    BusyProbe busy_probe_{};
    void* busy_context_{};
    filesystem::FileWriter writer_{};

    /** @brief Working storage of one transfer, on the heap only while it runs. */
    struct Buffers {
        std::array<char, 128> host{};
        std::array<char, 320> path{};
        std::array<char, 640> request{};
        std::array<char, 1024> headers{};
    };

    std::unique_ptr<Buffers> buffers_{};
    /** @brief Stays resident: status and the hosted loader read it after the transfer. */
    std::array<char, 192> destination_{};
    std::size_t request_size_{}, sent_{}, header_size_{};
    std::uint32_t started_at_{}, progress_at_{}, received_{}, expected_{};
    std::uint32_t polled_at_{}, cleanup_started_at_{};
    const char* result_{"idle"};
    const char* error_{""};
    bool cleanup_timed_out_{};
    std::uint16_t port_{80};
    State state_{State::idle};
};

} // namespace newmota
