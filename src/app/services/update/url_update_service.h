/*
  url_update_service.h - bounded asynchronous native-image URL update

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "core/update/update_session.h"
#include "newmota_features.h"
#include "newmota/platform/tcp_stream.h"
#include "services/update/url_update_limits.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace newmota {

class RestartController;
class Settings;

enum class UrlUpdateState : std::uint8_t {
    idle,
    handoff,
    waiting_network,
    closing_stream,
    connecting,
    sending_request,
    receiving_headers,
    downloading,
    restarting,
    failed,
};

enum class UrlUpdateRequestResult : std::uint8_t {
    started,
    handoff,
    busy,
    no_url,
    invalid_url,
    unavailable,
    failed,
};

/** @brief Pulls one operator-selected HTTP image into the native update writer. */
class UrlUpdateService final {
  public:
    using FilesystemBusyProbe = bool (*)(void* context);

    UrlUpdateService(Settings& settings, RestartController& restart, platform::TcpStream stream,
        UpdateSession* session, bool restart_after_failed_write);

    /** @brief Blocks update starts while a normal-app filesystem transfer owns storage. */
    void set_filesystem_busy_probe(FilesystemBusyProbe probe, void* context) {
        filesystem_busy_probe_ = probe;
        filesystem_busy_context_ = context;
    }

    /** @brief Starts a pull or schedules its Safeboot handoff. */
    [[nodiscard]] UrlUpdateRequestResult request(std::uint32_t now);
    /** @brief Advances network and writer work without blocking. */
#if USE_BACKLOG
    /** @brief As above; @p start_allowed false holds the start, as Tasmota's ota_state_flag
     * waits for CommandsReady(). */
    void poll(std::uint32_t now, bool network_available, bool start_allowed);
#else
    void poll(std::uint32_t now, bool network_available);
#endif

    /** @brief True while this service owns or is replacing the shared TCP stream. */
    [[nodiscard]] bool active() const;

    [[nodiscard]] UrlUpdateState state() const { return state_; }

    [[nodiscard]] const char* state_name() const;
    [[nodiscard]] const char* error_message() const;
    [[nodiscard]] std::uint32_t received_bytes() const;
    [[nodiscard]] std::uint32_t expected_bytes() const;
    [[nodiscard]] unsigned int progress_percent() const;

  private:
    enum class Failure : std::uint8_t {
        none,
        no_url,
        invalid_url,
        unsupported_scheme,
        unavailable,
#if !defined(NEWMOTA_IMAGE_SAFEBOOT)
        no_memory, ///< Application images only; Safeboot keeps static buffers.
#endif
        connect_failed,
        timeout,
        http_status,
        invalid_headers,
        unsupported_transfer,
        invalid_size,
        image_rejected,
        transport_failed,
        settings_failed,
        activation_failed,
        restart_failed,
    };

    [[nodiscard]] bool load_url();
    [[nodiscard]] bool build_request();
    [[nodiscard]] bool configure_writer();
    [[nodiscard]] bool begin_handoff(std::uint32_t now);
    [[nodiscard]] bool begin_download(std::uint32_t now);
    [[nodiscard]] bool parse_headers(std::uint32_t& content_length);
    [[nodiscard]] bool consume_body(const std::uint8_t* bytes, std::size_t size, std::uint32_t now);
    [[nodiscard]] bool flush_body(std::uint32_t now);
    void complete_download(std::uint32_t now);
    void fail(Failure failure, std::uint32_t now);
    void report_progress();

    Settings& settings_;
    RestartController& restart_;
    platform::TcpStream stream_{};
    UpdateSession* session_{};

    /** @brief Working storage of one update, on the heap only while it runs. */
    struct Buffers {
        std::array<char, 128> host{};
        std::array<char, 194> path{};
        std::array<char, 512> request{};
        std::array<char, 768> response_headers{};
        std::array<std::uint8_t, url_update_detail::kBodyBufferCapacity> body{};
    };

    /** @brief Allocates the working storage before the URL is parsed into it. */
    [[nodiscard]] bool allocate_buffers();
    /** @brief Frees the working storage once the update no longer needs it. */
    void release_buffers();
#if defined(NEWMOTA_IMAGE_SAFEBOOT)
    // Safeboot keeps them static: its binary size matters more than its free heap.
    Buffers buffers_storage_{};

    [[nodiscard]] Buffers& buffers() { return buffers_storage_; }
#else
    std::unique_ptr<Buffers> buffers_{};

    [[nodiscard]] Buffers& buffers() { return *buffers_; }
#endif
    std::size_t host_size_{};
    std::size_t path_size_{};
    std::size_t request_size_{};
    std::size_t request_sent_{};
    std::size_t response_headers_size_{};
    std::size_t body_buffer_size_{};
    std::size_t writer_chunk_capacity_{};
    FilesystemBusyProbe filesystem_busy_probe_{};
    void* filesystem_busy_context_{};
    std::uint16_t port_{};
    std::uint32_t body_received_{};
    std::uint32_t expected_image_size_{};
    std::uint32_t last_progress_at_{};
    std::uint32_t command_start_deadline_{};
    std::uint32_t operation_started_at_{};
    unsigned int reported_progress_{};
    UrlUpdateState state_{UrlUpdateState::idle};
    Failure failure_{Failure::none};
    bool restart_after_failed_write_{};
    bool writer_activity_{};
};

} // namespace newmota
