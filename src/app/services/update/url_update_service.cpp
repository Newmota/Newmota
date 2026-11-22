/*
  url_update_service.cpp - bounded asynchronous native-image URL update

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file url_update_service.cpp
 * @brief Cooperative native firmware download and activation.
 */

#include "services/update/url_update_service.h"
#include "services/update/url_update_limits.h"

#include "core/system/restart.h"
#include "newmota/log.h"
#include "services/settings/settings.h"
#include "support/number.h"
#include "support/text.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <limits>
#include <string_view>

namespace newmota {
namespace {

constexpr std::uint32_t kCommandResponseGraceMilliseconds = 1200U;
constexpr std::uint32_t kInactivityTimeoutMilliseconds = 30000U;
constexpr std::uint32_t kStreamCloseTimeoutMilliseconds = 5000U;
constexpr unsigned int kProgressStepPercent = 10U;

[[nodiscard]] bool deadline_reached(const std::uint32_t now, const std::uint32_t deadline) {
    return static_cast<std::int32_t>(now - deadline) >= 0;
}

[[nodiscard]] bool is_visible_url_byte(const char character) {
    const auto value = static_cast<unsigned char>(character);
    return value >= 0x21U && value <= 0x7eU;
}

[[nodiscard]] bool valid_host_character(const char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '.' || character == '-' ||
           character == '_';
}

[[nodiscard]] bool valid_path_character(const char character) {
    if (!is_visible_url_byte(character))
        return false;
    switch (character) {
    case '"':
    case '#':
    case '<':
    case '>':
    case '\\':
    case '^':
    case '`':
    case '{':
    case '|':
    case '}':
        return false;
    default:
        return true;
    }
}

[[nodiscard]] bool valid_header_text(const std::string_view value) {
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if ((byte < 0x20U && character != '\r' && character != '\n' && character != '\t') ||
            byte == 0x7fU) {
            return false;
        }
    }
    return true;
}

} // namespace

UrlUpdateService::UrlUpdateService(Settings& settings, RestartController& restart,
    const platform::TcpStream stream, UpdateSession* const session,
    const bool restart_after_failed_write)
    : settings_(settings), restart_(restart), stream_(stream), session_(session),
      restart_after_failed_write_(restart_after_failed_write) {}

/** @brief Validates and stores the bounded chunk size for the selected writer. */
bool UrlUpdateService::configure_writer() {
    writer_chunk_capacity_ = 0U;
    if (session_ == nullptr || !session_->writer_available())
        return false;
    const auto maximum = session_->maximum_chunk_size();
    const auto minimum = session_->minimum_initial_chunk_size();
    if (!url_update_detail::writer_chunk_limits_are_usable(
            maximum, minimum, buffers().body.size())) {
        return false;
    }
    writer_chunk_capacity_ =
        url_update_detail::bounded_writer_chunk_capacity(maximum, buffers().body.size());
    return true;
}

/** @brief Allocates the working storage; Safeboot keeps it static. */
bool UrlUpdateService::allocate_buffers() {
#if defined(NEWMOTA_IMAGE_SAFEBOOT)
    return true;
#else
    buffers_.reset(new (std::nothrow) Buffers{});
    if (buffers_)
        return true;
    failure_ = Failure::no_memory;
    state_ = UrlUpdateState::failed;
    AddLog(LogLevel::error, "OTA: URL update failed: %s", error_message());
    return false;
#endif
}

/** @brief Frees the working storage in application images. */
void UrlUpdateService::release_buffers() {
#if !defined(NEWMOTA_IMAGE_SAFEBOOT)
    buffers_.reset();
#endif
}

/** @brief Parses only an operator-selected plain HTTP URL into bounded fields. */
bool UrlUpdateService::load_url() {
    buffers().host.fill('\0');
    buffers().path.fill('\0');
    host_size_ = 0U;
    path_size_ = 0U;
    port_ = 80U;
    const auto url = settings_.ota_url();
    if (url.empty()) {
        failure_ = Failure::no_url;
        return false;
    }
    if (support::ascii_iequals(url.substr(0U, 8U), "https://")) {
        failure_ = Failure::unsupported_scheme;
        return false;
    }
    if (!support::ascii_iequals(url.substr(0U, 7U), "http://") ||
        url.find('#') != std::string_view::npos) {
        failure_ = Failure::invalid_url;
        return false;
    }

    const auto remainder = url.substr(7U);
    const auto authority_end = remainder.find_first_of("/?");
    const auto authority = remainder.substr(0U, authority_end);
    if (authority.empty() || authority.find('@') != std::string_view::npos ||
        authority.find('[') != std::string_view::npos ||
        authority.find(']') != std::string_view::npos) {
        failure_ = Failure::invalid_url;
        return false;
    }

    const auto colon = authority.find(':');
    const auto host = authority.substr(0U, colon);
    if (host.empty() || host.size() >= buffers().host.size()) {
        failure_ = Failure::invalid_url;
        return false;
    }
    for (const auto character : host) {
        if (!valid_host_character(character)) {
            failure_ = Failure::invalid_url;
            return false;
        }
    }
    std::memcpy(buffers().host.data(), host.data(), host.size());
    host_size_ = host.size();

    if (colon != std::string_view::npos) {
        if (authority.find(':', colon + 1U) != std::string_view::npos) {
            failure_ = Failure::invalid_url;
            return false;
        }
        std::uint32_t port = 0U;
        if (!support::parse_unsigned_decimal(authority.substr(colon + 1U), UINT16_MAX, port) ||
            port == 0U) {
            failure_ = Failure::invalid_url;
            return false;
        }
        port_ = static_cast<std::uint16_t>(port);
    }

    std::string_view path;
    if (authority_end == std::string_view::npos) {
        path = "/";
    } else {
        path = remainder.substr(authority_end);
        if (!path.empty() && path.front() == '?') {
            buffers().path[0] = '/';
            std::memcpy(buffers().path.data() + 1U, path.data(), path.size());
            path_size_ = path.size() + 1U;
            path = {};
        }
    }
    if (!path.empty()) {
        if (path.size() >= buffers().path.size()) {
            failure_ = Failure::invalid_url;
            return false;
        }
        for (const auto character : path) {
            if (!valid_path_character(character)) {
                failure_ = Failure::invalid_url;
                return false;
            }
        }
        std::memcpy(buffers().path.data(), path.data(), path.size());
        path_size_ = path.size();
    }
    if (path_size_ == 0U) {
        failure_ = Failure::invalid_url;
        return false;
    }
    buffers().path[path_size_] = '\0';
    return true;
}

/** @brief Starts an update by switching to the paired Safeboot image. */
bool UrlUpdateService::begin_handoff(const std::uint32_t now) {
    const auto saved = settings_.set_ota_update_pending(true);
    if (saved == SettingsUpdateResult::failed || saved == SettingsUpdateResult::invalid) {
        failure_ = Failure::settings_failed;
        state_ = UrlUpdateState::failed;
        return false;
    }
    switch (restart_.schedule(RestartTarget::alternate, now)) {
    case RestartRequestResult::scheduled:
        state_ = UrlUpdateState::handoff;
        AddLog(LogLevel::info, "OTA: switching to Safeboot for OtaUrl update");
        return true;
    case RestartRequestResult::unavailable:
        failure_ = Failure::unavailable;
        break;
    case RestartRequestResult::failed:
        failure_ = Failure::restart_failed;
        break;
    case RestartRequestResult::busy:
        failure_ = Failure::restart_failed;
        break;
    }
    (void)settings_.set_ota_update_pending(false);
    state_ = UrlUpdateState::failed;
    return false;
}

/** @brief Starts one command-requested pull or its recovery handoff. */
UrlUpdateRequestResult UrlUpdateService::request(const std::uint32_t now) {
    if (filesystem_busy_probe_ != nullptr && filesystem_busy_probe_(filesystem_busy_context_)) {
        return UrlUpdateRequestResult::busy;
    }
    if (active() || state_ == UrlUpdateState::handoff ||
        state_ == UrlUpdateState::waiting_network || restart_.pending()) {
        return UrlUpdateRequestResult::busy;
    }
    state_ = UrlUpdateState::idle;
    failure_ = Failure::none;
    expected_image_size_ = 0U;
    body_received_ = 0U;
    body_buffer_size_ = 0U;
    response_headers_size_ = 0U;
    request_size_ = 0U;
    request_sent_ = 0U;
    reported_progress_ = 0U;
    writer_activity_ = false;
    if (!allocate_buffers())
        return UrlUpdateRequestResult::unavailable;
    if (!load_url()) {
        release_buffers();
        state_ = UrlUpdateState::failed;
        return failure_ == Failure::no_url ? UrlUpdateRequestResult::no_url
                                           : UrlUpdateRequestResult::invalid_url;
    }
    if (session_ == nullptr) {
        // Safeboot downloads; this image only validated the URL.
        release_buffers();
        return begin_handoff(now) ? UrlUpdateRequestResult::handoff
                                  : UrlUpdateRequestResult::unavailable;
    }
    if (!configure_writer()) {
        release_buffers();
        failure_ = Failure::unavailable;
        state_ = UrlUpdateState::failed;
        return UrlUpdateRequestResult::unavailable;
    }
    const auto saved = settings_.set_ota_update_pending(true);
    if (saved == SettingsUpdateResult::failed || saved == SettingsUpdateResult::invalid) {
        release_buffers();
        failure_ = Failure::settings_failed;
        state_ = UrlUpdateState::failed;
        return UrlUpdateRequestResult::failed;
    }
    state_ = UrlUpdateState::waiting_network;
    command_start_deadline_ = now + kCommandResponseGraceMilliseconds;
    operation_started_at_ = now;
    last_progress_at_ = now;
    AddLog(LogLevel::info, "OTA: URL update requested");
    return UrlUpdateRequestResult::started;
}

/** @brief Builds one bounded HTTP/1.1 request without URL user information. */
bool UrlUpdateService::build_request() {
    std::array<char, 140> host_header{};
    const int host_length = port_ == 80U
                                ? std::snprintf(host_header.data(), host_header.size(), "%.*s",
                                      static_cast<int>(host_size_), buffers().host.data())
                                : std::snprintf(host_header.data(), host_header.size(), "%.*s:%u",
                                      static_cast<int>(host_size_), buffers().host.data(),
                                      static_cast<unsigned int>(port_));
    if (host_length < 0 || static_cast<std::size_t>(host_length) >= host_header.size()) {
        return false;
    }
    const int request_length = std::snprintf(buffers().request.data(), buffers().request.size(),
        "GET %s HTTP/1.1\r\nHost: %s\r\n"
        "Accept: application/octet-stream\r\n"
        "Accept-Encoding: identity\r\n"
        "Connection: close\r\nUser-Agent: Newmota-OTA\r\n\r\n",
        buffers().path.data(), host_header.data());
    if (request_length < 0 ||
        static_cast<std::size_t>(request_length) >= buffers().request.size()) {
        return false;
    }
    request_size_ = static_cast<std::size_t>(request_length);
    request_sent_ = 0U;
    return true;
}

/** @brief Drops the pending marker before opening a native flash writer. */
bool UrlUpdateService::begin_download(const std::uint32_t now) {
    if (settings_.ota_update_pending() &&
        settings_.set_ota_update_pending(false) == SettingsUpdateResult::failed) {
        fail(Failure::settings_failed, now);
        return false;
    }
    if (!build_request() || stream_.close == nullptr || stream_.state == nullptr ||
        stream_.open == nullptr || stream_.read == nullptr || stream_.write == nullptr) {
        fail(Failure::unavailable, now);
        return false;
    }
    stream_.close(stream_.context);
    state_ = UrlUpdateState::closing_stream;
    operation_started_at_ = now;
    last_progress_at_ = now;
    return true;
}

/** @brief Reads and validates the bounded status line and response headers. */
bool UrlUpdateService::parse_headers(std::uint32_t& content_length) {
    const std::string_view response{buffers().response_headers.data(), response_headers_size_};
    const auto separator = response.find("\r\n\r\n");
    if (separator == std::string_view::npos) {
        failure_ = Failure::invalid_headers;
        return false;
    }
    const auto headers = response.substr(0U, separator);
    if (!valid_header_text(headers)) {
        failure_ = Failure::invalid_headers;
        return false;
    }
    const auto status_end = headers.find("\r\n");
    if (status_end == std::string_view::npos) {
        failure_ = Failure::invalid_headers;
        return false;
    }
    const auto status = headers.substr(0U, status_end);
    if (status.size() < 12U || status.substr(0U, 7U) != "HTTP/1." ||
        (status[7] != '0' && status[7] != '1') || status[8] != ' ' ||
        (status.size() > 12U && status[12] != ' ')) {
        failure_ = Failure::invalid_headers;
        return false;
    }
    std::uint32_t status_code = 0U;
    if (!support::parse_unsigned_decimal(status.substr(9U, 3U), 999U, status_code)) {
        failure_ = Failure::invalid_headers;
        return false;
    }
    if (status_code != 200U) {
        failure_ = Failure::http_status;
        return false;
    }

    bool content_length_seen = false;
    bool transfer_encoding_seen = false;
    std::size_t position = status_end + 2U;
    while (position < headers.size()) {
        const auto end = headers.find("\r\n", position);
        const auto line = end == std::string_view::npos ? headers.substr(position)
                                                        : headers.substr(position, end - position);
        position = end == std::string_view::npos ? headers.size() : end + 2U;
        if (line.empty())
            break;
        if (line.front() == ' ' || line.front() == '\t') {
            failure_ = Failure::invalid_headers;
            return false;
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0U) {
            failure_ = Failure::invalid_headers;
            return false;
        }
        const auto name = support::trim_ascii(line.substr(0U, colon));
        const auto value = support::trim_ascii(line.substr(colon + 1U));
        if (support::ascii_iequals(name, "content-length")) {
            if (content_length_seen ||
                !support::parse_unsigned_decimal(
                    value, std::numeric_limits<std::uint32_t>::max(), content_length)) {
                failure_ = Failure::invalid_headers;
                return false;
            }
            content_length_seen = true;
        } else if (support::ascii_iequals(name, "transfer-encoding")) {
            if (!value.empty())
                transfer_encoding_seen = true;
        } else if (support::ascii_iequals(name, "content-encoding") && !value.empty() &&
                   !support::ascii_iequals(value, "identity")) {
            failure_ = Failure::unsupported_transfer;
            return false;
        }
    }
    if (transfer_encoding_seen || !content_length_seen) {
        failure_ = Failure::unsupported_transfer;
        return false;
    }
    if (content_length == 0U || session_ == nullptr ||
        content_length > session_->maximum_image_size()) {
        failure_ = Failure::invalid_size;
        return false;
    }
    const auto started = session_->begin(content_length);
    if (started != UpdateSessionResult::success) {
        failure_ = started == UpdateSessionResult::invalid_size
                       ? Failure::invalid_size
                       : (started == UpdateSessionResult::unavailable ? Failure::unavailable
                                                                      : Failure::image_rejected);
        return false;
    }
    expected_image_size_ = content_length;
    body_received_ = 0U;
    body_buffer_size_ = 0U;
    state_ = UrlUpdateState::downloading;
    return true;
}

/** @brief Sends one buffered body prefix when the native writer can accept it. */
bool UrlUpdateService::flush_body(const std::uint32_t now) {
    if (body_buffer_size_ == 0U || session_ == nullptr)
        return true;
    if (session_->received_bytes() == 0U &&
        body_buffer_size_ < session_->minimum_initial_chunk_size()) {
        return true;
    }
    writer_activity_ = true;
    const auto written = session_->write(session_->received_bytes(),
        {reinterpret_cast<const char*>(buffers().body.data()), body_buffer_size_});
    if (written != UpdateSessionResult::success) {
        failure_ = written == UpdateSessionResult::invalid_size ||
                           written == UpdateSessionResult::invalid_chunk
                       ? Failure::invalid_size
                       : Failure::image_rejected;
        return false;
    }
    body_buffer_size_ = 0U;
    last_progress_at_ = now;
    report_progress();
    return true;
}

/** @brief Streams body bytes through one fixed-size writer chunk. */
bool UrlUpdateService::consume_body(
    const std::uint8_t* const bytes, const std::size_t size, const std::uint32_t now) {
    if ((bytes == nullptr && size != 0U) || session_ == nullptr ||
        size > expected_image_size_ - body_received_) {
        failure_ = Failure::invalid_size;
        return false;
    }
    const auto maximum_chunk = writer_chunk_capacity_;
    if (maximum_chunk == 0U || maximum_chunk > buffers().body.size()) {
        failure_ = Failure::unavailable;
        return false;
    }
    std::size_t offset = 0U;
    while (offset < size) {
        if (body_buffer_size_ == maximum_chunk && !flush_body(now)) {
            return false;
        }
        const auto available = maximum_chunk - body_buffer_size_;
        const auto amount = std::min(available, size - offset);
        std::memcpy(buffers().body.data() + body_buffer_size_, bytes + offset, amount);
        body_buffer_size_ += amount;
        body_received_ += static_cast<std::uint32_t>(amount);
        offset += amount;
        if ((session_->received_bytes() == 0U &&
                body_buffer_size_ >= session_->minimum_initial_chunk_size()) ||
            session_->received_bytes() != 0U || body_buffer_size_ == maximum_chunk) {
            if (!flush_body(now))
                return false;
        }
    }
    last_progress_at_ = now;
    if (body_received_ == expected_image_size_) {
        if (!flush_body(now) || session_->received_bytes() != expected_image_size_) {
            if (failure_ == Failure::none)
                failure_ = Failure::invalid_size;
            return false;
        }
        complete_download(now);
    }
    return true;
}

/** @brief Verifies and activates the native image, then schedules its reboot. */
void UrlUpdateService::complete_download(const std::uint32_t now) {
    state_ = UrlUpdateState::restarting;
    release_buffers();
    if (session_ == nullptr || session_->finish() != UpdateSessionResult::success) {
        fail(Failure::image_rejected, now);
        return;
    }
    AddLog(LogLevel::info, "OTA: image verified (%lu bytes)",
        static_cast<unsigned long>(expected_image_size_));
    if (session_->activate() != UpdateSessionResult::success) {
        fail(Failure::activation_failed, now);
        return;
    }
    stream_.close(stream_.context);
    switch (restart_.schedule(RestartTarget::current, now)) {
    case RestartRequestResult::scheduled:
        state_ = UrlUpdateState::restarting;
        AddLog(LogLevel::info, "OTA: update activated; restarting");
        return;
    case RestartRequestResult::unavailable:
    case RestartRequestResult::failed:
    case RestartRequestResult::busy:
        failure_ = Failure::restart_failed;
        state_ = UrlUpdateState::failed;
        AddLog(LogLevel::error, "OTA: activation succeeded but restart scheduling failed");
        if (!restart_.pending())
            restart_.restart_now();
        return;
    }
}

/** @brief Releases a failed attempt and preserves Safeboot as the recovery path. */
void UrlUpdateService::fail(const Failure failure, const std::uint32_t now) {
    failure_ = failure;
    release_buffers();
    if (stream_.close != nullptr)
        stream_.close(stream_.context);
    if (session_ != nullptr) {
        (void)session_->abort();
    }
    if (settings_.ota_update_pending() && !writer_activity_) {
        (void)settings_.set_ota_update_pending(false);
    }
    state_ = UrlUpdateState::failed;
    AddLog(LogLevel::error, "OTA: URL update failed: %s", error_message());
    if (writer_activity_ && restart_after_failed_write_ && !restart_.pending()) {
        if (restart_.schedule(RestartTarget::current, now) == RestartRequestResult::scheduled) {
            AddLog(LogLevel::info, "OTA: restarting after interrupted native staging");
        }
    }
}

/** @brief Reports coarse progress without logging the possibly sensitive URL. */
void UrlUpdateService::report_progress() {
    if (expected_image_size_ == 0U)
        return;
    const auto percent = progress_percent();
    const auto bucket = percent / kProgressStepPercent * kProgressStepPercent;
    if (bucket < reported_progress_ || bucket == reported_progress_)
        return;
    reported_progress_ = bucket;
    AddLog(LogLevel::info, "OTA: downloaded %lu/%lu bytes (%u%%)",
        static_cast<unsigned long>(received_bytes()),
        static_cast<unsigned long>(expected_image_size_), bucket);
}

/** @brief Advances the active request from the application loop. */
#if USE_BACKLOG
void UrlUpdateService::poll(
    const std::uint32_t now, const bool network_available, const bool start_allowed) {
#else
void UrlUpdateService::poll(const std::uint32_t now, const bool network_available) {
#endif
    if (state_ == UrlUpdateState::idle && settings_.ota_update_pending()) {
        failure_ = Failure::none;
        expected_image_size_ = 0U;
        body_received_ = 0U;
        reported_progress_ = 0U;
        writer_activity_ = false;
        if (!allocate_buffers()) {
            fail(failure_, now);
            return;
        }
        if (!load_url()) {
            fail(failure_, now);
            return;
        }
        if (session_ == nullptr) {
            release_buffers();
            const auto cleared = settings_.set_ota_update_pending(false);
            const bool clear_failed =
                cleared == SettingsUpdateResult::failed || cleared == SettingsUpdateResult::invalid;
            failure_ = clear_failed ? Failure::settings_failed : Failure::unavailable;
            state_ = UrlUpdateState::failed;
            AddLog(LogLevel::error,
                clear_failed ? "OTA: recovery handoff did not start; clearing intent failed"
                             : "OTA: recovery handoff did not start; pending intent cleared");
            return;
        }
        if (!configure_writer()) {
            fail(Failure::unavailable, now);
            return;
        }
        state_ = UrlUpdateState::waiting_network;
        command_start_deadline_ = now;
        operation_started_at_ = now;
        last_progress_at_ = now;
    }

    if (state_ == UrlUpdateState::waiting_network) {
        if (!network_available || !deadline_reached(now, command_start_deadline_)) {
            return;
        }
#if USE_BACKLOG
        if (!start_allowed) {
            return;
        }
#endif
        (void)begin_download(now);
        return;
    }

    if (state_ == UrlUpdateState::closing_stream) {
        if (now - operation_started_at_ >= kStreamCloseTimeoutMilliseconds) {
            fail(Failure::connect_failed, now);
            return;
        }
        const auto native_state = stream_.state(stream_.context);
        if (native_state == platform::TcpStreamState::idle) {
            const auto opened =
                stream_.open(stream_.context, {buffers().host.data(), host_size_}, port_, false);
            if (opened == platform::TcpStreamOpenResult::accepted) {
                state_ = UrlUpdateState::connecting;
                last_progress_at_ = now;
            } else if (opened != platform::TcpStreamOpenResult::busy) {
                fail(Failure::connect_failed, now);
                return;
            }
        } else if (native_state == platform::TcpStreamState::unavailable) {
            fail(Failure::unavailable, now);
            return;
        }
        return;
    }

    if (state_ == UrlUpdateState::idle || state_ == UrlUpdateState::handoff ||
        state_ == UrlUpdateState::failed || state_ == UrlUpdateState::restarting ||
        state_ == UrlUpdateState::waiting_network) {
        return;
    }
    if (now - last_progress_at_ >= kInactivityTimeoutMilliseconds) {
        fail(Failure::timeout, now);
        return;
    }

    const auto native_state = stream_.state(stream_.context);
    if (state_ == UrlUpdateState::connecting) {
        if (native_state == platform::TcpStreamState::connected) {
            state_ = UrlUpdateState::sending_request;
            last_progress_at_ = now;
        } else if (native_state == platform::TcpStreamState::closing ||
                   native_state == platform::TcpStreamState::unavailable ||
                   native_state == platform::TcpStreamState::idle) {
            fail(Failure::connect_failed, now);
        }
        return;
    }

    if (native_state != platform::TcpStreamState::connected) {
        if (native_state == platform::TcpStreamState::closing ||
            native_state == platform::TcpStreamState::unavailable ||
            native_state == platform::TcpStreamState::idle) {
            fail(Failure::transport_failed, now);
        }
        return;
    }

    if (state_ == UrlUpdateState::sending_request) {
        const auto* const bytes =
            reinterpret_cast<const std::uint8_t*>(buffers().request.data() + request_sent_);
        const auto result = stream_.write(stream_.context, bytes, request_size_ - request_sent_);
        if (result.state == platform::TcpStreamIoState::progressed && result.size > 0U &&
            result.size <= request_size_ - request_sent_) {
            request_sent_ += result.size;
            last_progress_at_ = now;
            if (request_sent_ == request_size_) {
                state_ = UrlUpdateState::receiving_headers;
                response_headers_size_ = 0U;
            }
        } else if (result.state != platform::TcpStreamIoState::would_block) {
            fail(Failure::transport_failed, now);
        }
        return;
    }

    if (state_ == UrlUpdateState::receiving_headers) {
        if (response_headers_size_ == buffers().response_headers.size()) {
            fail(Failure::invalid_headers, now);
            return;
        }
        auto* const destination = reinterpret_cast<std::uint8_t*>(
            buffers().response_headers.data() + response_headers_size_);
        const auto capacity = buffers().response_headers.size() - response_headers_size_;
        const auto read = stream_.read(stream_.context, destination, capacity);
        if (read.state == platform::TcpStreamIoState::progressed && read.size > 0U &&
            read.size <= capacity) {
            response_headers_size_ += read.size;
            last_progress_at_ = now;
            const std::string_view current{
                buffers().response_headers.data(), response_headers_size_};
            const auto end = current.find("\r\n\r\n");
            if (end != std::string_view::npos) {
                std::uint32_t content_length = 0U;
                if (!parse_headers(content_length)) {
                    fail(failure_, now);
                    return;
                }
                const auto body_offset = end + 4U;
                const auto trailing = response_headers_size_ - body_offset;
                if (trailing > 0U &&
                    !consume_body(reinterpret_cast<const std::uint8_t*>(
                                      buffers().response_headers.data() + body_offset),
                        trailing, now)) {
                    fail(failure_, now);
                    return;
                }
                response_headers_size_ = 0U;
            }
        } else if (read.state == platform::TcpStreamIoState::closed) {
            fail(Failure::transport_failed, now);
        } else if (read.state != platform::TcpStreamIoState::would_block) {
            fail(Failure::transport_failed, now);
        }
        return;
    }

    if (state_ == UrlUpdateState::downloading) {
        const auto maximum_chunk = writer_chunk_capacity_;
        if (maximum_chunk == 0U || maximum_chunk > buffers().body.size()) {
            fail(Failure::unavailable, now);
            return;
        }
        if (body_buffer_size_ == maximum_chunk && !flush_body(now)) {
            fail(failure_, now);
            return;
        }
        const auto remaining = expected_image_size_ - body_received_;
        const auto capacity =
            std::min(maximum_chunk - body_buffer_size_, static_cast<std::size_t>(remaining));
        if (capacity == 0U) {
            if (!flush_body(now) || session_->received_bytes() != expected_image_size_) {
                fail(Failure::invalid_size, now);
                return;
            }
            complete_download(now);
            return;
        }
        const auto read =
            stream_.read(stream_.context, buffers().body.data() + body_buffer_size_, capacity);
        if (read.state == platform::TcpStreamIoState::progressed && read.size > 0U &&
            read.size <= capacity) {
            body_buffer_size_ += read.size;
            body_received_ += static_cast<std::uint32_t>(read.size);
            if ((session_->received_bytes() == 0U &&
                    body_buffer_size_ >= session_->minimum_initial_chunk_size()) ||
                session_->received_bytes() != 0U || body_buffer_size_ == maximum_chunk) {
                if (!flush_body(now)) {
                    fail(failure_, now);
                    return;
                }
            }
            last_progress_at_ = now;
            if (body_received_ == expected_image_size_) {
                if (!flush_body(now) || session_->received_bytes() != expected_image_size_) {
                    fail(Failure::invalid_size, now);
                    return;
                }
                complete_download(now);
            }
        } else if (read.state == platform::TcpStreamIoState::closed) {
            fail(Failure::transport_failed, now);
        } else if (read.state != platform::TcpStreamIoState::would_block) {
            fail(Failure::transport_failed, now);
        }
    }
}

/** @brief Reports whether the asynchronous updater currently owns the stream. */
bool UrlUpdateService::active() const {
    return state_ == UrlUpdateState::closing_stream || state_ == UrlUpdateState::connecting ||
           state_ == UrlUpdateState::sending_request ||
           state_ == UrlUpdateState::receiving_headers || state_ == UrlUpdateState::downloading ||
           state_ == UrlUpdateState::restarting;
}

/** @brief Returns the current progress counters. */
std::uint32_t UrlUpdateService::received_bytes() const {
    return body_received_;
}

std::uint32_t UrlUpdateService::expected_bytes() const {
    return expected_image_size_;
}

unsigned int UrlUpdateService::progress_percent() const {
    if (expected_image_size_ == 0U)
        return 0U;
    const auto received = std::min(body_received_, expected_image_size_);
    return static_cast<unsigned int>(
        static_cast<std::uint64_t>(received) * 100U / expected_image_size_);
}

const char* UrlUpdateService::state_name() const {
    switch (state_) {
    case UrlUpdateState::idle:
        return "idle";
    case UrlUpdateState::handoff:
        return "switching-to-safeboot";
    case UrlUpdateState::waiting_network:
        return "waiting-for-network";
    case UrlUpdateState::closing_stream:
        return "preparing-connection";
    case UrlUpdateState::connecting:
        return "connecting";
    case UrlUpdateState::sending_request:
        return "requesting-image";
    case UrlUpdateState::receiving_headers:
        return "receiving-headers";
    case UrlUpdateState::downloading:
        return "downloading";
    case UrlUpdateState::restarting:
        return "restarting";
    case UrlUpdateState::failed:
        return "failed";
    }
    return "failed";
}

const char* UrlUpdateService::error_message() const {
    switch (failure_) {
    case Failure::none:
        return "";
    case Failure::no_url:
        return "OtaUrl is not configured";
    case Failure::invalid_url:
        return "OtaUrl is invalid";
    case Failure::unsupported_scheme:
        return "HTTPS URL updates are unavailable";
    case Failure::unavailable:
        return "Native update path is unavailable";
#if !defined(NEWMOTA_IMAGE_SAFEBOOT)
    case Failure::no_memory:
        return "Out of memory";
#endif
    case Failure::connect_failed:
        return "HTTP connection failed";
    case Failure::timeout:
        return "HTTP update timed out";
    case Failure::http_status:
        return "HTTP server did not return status 200";
    case Failure::invalid_headers:
        return "HTTP response headers are invalid";
    case Failure::unsupported_transfer:
        return "HTTP response needs a direct Content-Length body";
    case Failure::invalid_size:
        return "Image size is outside the update limit";
    case Failure::image_rejected:
        return "Image failed native target or integrity validation";
    case Failure::transport_failed:
        return "HTTP transfer ended early";
    case Failure::settings_failed:
        return "Update state could not be saved";
    case Failure::activation_failed:
        return "Verified image could not be activated";
    case Failure::restart_failed:
        return "Image activated but restart failed";
    }
    return "Update failed";
}

} // namespace newmota
