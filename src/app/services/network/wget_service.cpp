/*
  wget_service.cpp - bounded asynchronous HTTP file download

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#include "services/network/wget_service.h"

#include "newmota/log.h"
#include "newmota/platform/platform.h"
#include "support/number.h"
#include "support/text.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>

namespace newmota {
namespace {
constexpr char kStagingPath[] = "/.newmota/wget.part";
constexpr std::uint32_t kIdleTimeout = 30000U;
constexpr std::uint32_t kTotalTimeout = 300000U;

/** @brief Accepts only DNS/IPv4 host bytes supported by the shared native stream. */
bool host_character(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '-' || c == '_';
}
} // namespace

/** @brief Validates one HTTP URL before building a bounded identity-encoding request. */
bool WgetService::load_url(std::string_view url) {
    if (!support::ascii_iequals(url.substr(0, 7), "http://") ||
        url.find('#') != std::string_view::npos)
        return false;
    for (const auto c : url) {
        if (static_cast<unsigned char>(c) <= 0x20U || static_cast<unsigned char>(c) >= 0x7fU ||
            c == '\\' || c == '"' || c == '<' || c == '>')
            return false;
    }
    const auto remainder = url.substr(7);
    const auto end = remainder.find_first_of("/?");
    const auto authority = remainder.substr(0, end);
    const auto colon = authority.find(':');
    const auto host = authority.substr(0, colon);
    if (host.empty() || host.size() >= buffers_->host.size())
        return false;
    for (const auto c : host)
        if (!host_character(c))
            return false;
    std::uint32_t port = 80;
    if (colon != std::string_view::npos &&
        (!support::parse_unsigned_decimal(authority.substr(colon + 1), UINT16_MAX, port) ||
            port == 0))
        return false;
    auto path = end == std::string_view::npos ? std::string_view{"/"} : remainder.substr(end);
    const bool prepend_slash = path.front() == '?';
    if (path.size() + (prepend_slash ? 1U : 0U) >= buffers_->path.size())
        return false;
    buffers_->host.fill('\0');
    std::memcpy(buffers_->host.data(), host.data(), host.size());
    buffers_->path.fill('\0');
    if (prepend_slash)
        buffers_->path[0] = '/';
    std::memcpy(buffers_->path.data() + (prepend_slash ? 1U : 0U), path.data(), path.size());
    port_ = static_cast<std::uint16_t>(port);
    const std::string_view target{buffers_->path.data()};
    const auto resource = target.substr(0, target.find('?'));
    auto basename = resource.substr(resource.find_last_of('/') + 1U);
    if (basename.empty())
        basename = "index.html";
    if (basename == "." || basename == ".." || basename == ".newmota" ||
        basename.size() + 1U >= destination_.size())
        return false;
    destination_.fill('\0');
    destination_[0] = '/';
    std::memcpy(destination_.data() + 1, basename.data(), basename.size());
    // Reserve enough space for every field before formatting the request.
    const auto length = std::snprintf(buffers_->request.data(), buffers_->request.size(),
        "GET %s HTTP/1.1\r\nHost: %s:%u\r\nAccept-Encoding: identity\r\n"
        "Connection: close\r\nUser-Agent: Newmota-wget\r\n\r\n",
        buffers_->path.data(), buffers_->host.data(), static_cast<unsigned int>(port_));
    if (length < 0 || static_cast<std::size_t>(length) >= buffers_->request.size())
        return false;
    request_size_ = static_cast<std::size_t>(length);
    sent_ = 0;
    header_size_ = 0;
    return true;
}

/** @brief Reports retained status or reserves one transfer after checking application leases. */
CommandStatus WgetService::command(const CommandRequest& request, CommandOutput& output) {
    if (request.has_index)
        return CommandStatus::invalid;
    if (request.is_query()) {
        return output.format("{\"wget\":{\"State\":\"%s\",\"Destination\":", state_name()) &&
                       output.json_string(destination_.data()) &&
                       output.format(",\"Received\":%lu,\"Total\":%lu,\"Error\":",
                           static_cast<unsigned long>(received_),
                           static_cast<unsigned long>(expected_)) &&
                       output.json_string(error_) && output.write("}}")
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    }
    if (request.payload.empty())
        return CommandStatus::invalid;
    const auto started = start(request.payload);
    if (started == StartResult::busy) {
        (void)output.write("{\"wget\":\"Busy\"}");
        return CommandStatus::failed;
    }
    if (started == StartResult::invalid_url) {
        (void)output.write("{\"wget\":\"Invalid HTTP URL\"}");
        return CommandStatus::invalid;
    }
    if (started == StartResult::unavailable) {
        (void)output.write("{\"wget\":\"Unavailable\"}");
        return CommandStatus::failed;
    }
    if (started == StartResult::no_memory) {
        (void)output.write("{\"wget\":\"Out of memory\"}");
        return CommandStatus::failed;
    }
    return output.write("{\"wget\":\"Started\"}") ? CommandStatus::handled : CommandStatus::failed;
}

WgetService::StartResult WgetService::start(const std::string_view url) {
    if (active() || (busy_probe_ != nullptr && busy_probe_(busy_context_)))
        return StartResult::busy;
    buffers_.reset(new (std::nothrow) Buffers{});
    if (!buffers_)
        return StartResult::no_memory;
    if (!load_url(url)) {
        buffers_.reset();
        return StartResult::invalid_url;
    }
    if (!filesystem::is_mounted() || stream_.state == nullptr || stream_.open == nullptr ||
        stream_.read == nullptr || stream_.write == nullptr || stream_.close == nullptr) {
        buffers_.reset();
        return StartResult::unavailable;
    }
    started_at_ = progress_at_ = polled_at_ = platform::monotonic_milliseconds();
    result_ = "idle";
    error_ = "";
    cleanup_timed_out_ = false;
    received_ = expected_ = 0;
    state_ = State::preparing;
    return StartResult::started;
}

bool WgetService::completed() const {
    return !active() && std::strcmp(result_, "completed") == 0;
}

/** @brief Discards any unfinished staging file and drains the native connection. */
void WgetService::release() {
    if (writer_.native != nullptr)
        (void)filesystem::close(writer_);
    (void)filesystem::remove_if_exists(kStagingPath);
    stream_.close(stream_.context);
    buffers_.reset();
    cleanup_started_at_ = polled_at_;
    state_ = State::draining;
}

/** @brief Records the failure without changing the previously committed destination. */
void WgetService::fail(const char* reason) {
    result_ = "failed";
    error_ = reason;
    AddLog(LogLevel::error, "WGET: failed: %s", reason);
    release();
}

/** @brief Commits only a complete synchronized staging file with LittleFS atomic rename. */
void WgetService::finish() {
    const bool closed = filesystem::close(writer_);
    if (!closed || filesystem::rename(kStagingPath, destination_.data()) !=
                       filesystem::MutationResult::succeeded) {
        fail("File commit failed");
        return;
    }
    result_ = "completed";
    AddLog(LogLevel::info, "WGET: saved %s (%lu bytes)", destination_.data(),
        static_cast<unsigned long>(received_));
    release();
}

/** @brief Applies one received slice within the declared response length. */
bool WgetService::consume(const std::uint8_t* bytes, std::size_t size) {
    if (size > expected_ - received_ ||
        filesystem::write(writer_, bytes, size) != filesystem::MutationResult::succeeded) {
        fail("Body length or filesystem write failed");
        return false;
    }
    received_ += static_cast<std::uint32_t>(size);
    if (received_ == expected_)
        state_ = State::checking_end;
    return true;
}

/** @brief Parses bounded HTTP framing and opens staging only for a direct response body. */
bool WgetService::parse_headers() {
    const std::string_view response{buffers_->headers.data(), header_size_};
    const auto separator = response.find("\r\n\r\n");
    const auto headers = response.substr(0, separator);
    const auto status_end = headers.find("\r\n");
    const auto status = headers.substr(0, status_end);
    if (status.size() < 12 || status.substr(0, 7) != "HTTP/1." ||
        (status[7] != '0' && status[7] != '1') || status[8] != ' ' ||
        (status.size() > 12 && status[12] != ' ')) {
        fail("Invalid HTTP status line");
        return false;
    }
    std::uint32_t code = 0;
    if (!support::parse_unsigned_decimal(status.substr(9, 3), 999, code)) {
        fail("Invalid HTTP status");
        return false;
    }
    for (const auto c : headers) {
        if ((static_cast<unsigned char>(c) < 0x20U && c != '\r' && c != '\n' && c != '\t') ||
            static_cast<unsigned char>(c) >= 0x7fU) {
            fail("Invalid HTTP header text");
            return false;
        }
    }
    bool length_seen = false;
    bool unsupported = false;
    auto position = status_end == std::string_view::npos ? headers.size() : status_end + 2U;
    while (position < headers.size()) {
        const auto end = headers.find("\r\n", position);
        const auto line = headers.substr(
            position, end == std::string_view::npos ? headers.size() - position : end - position);
        position = end == std::string_view::npos ? headers.size() : end + 2;
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0 || line.front() == ' ' ||
            line.front() == '\t') {
            fail("Invalid HTTP header");
            return false;
        }
        const auto name = line.substr(0, colon);
        const auto value = support::trim_ascii(line.substr(colon + 1));
        if (support::ascii_iequals(name, "content-length")) {
            if (length_seen || !support::parse_unsigned_decimal(
                                   value, std::numeric_limits<std::uint32_t>::max(), expected_)) {
                fail("Invalid Content-Length");
                return false;
            }
            length_seen = true;
        } else if (support::ascii_iequals(name, "transfer-encoding")) {
            unsupported = true;
        } else if (support::ascii_iequals(name, "content-encoding") &&
                   !support::ascii_iequals(value, "identity")) {
            unsupported = true;
        } else if (support::ascii_iequals(name, "location")) {
            fail("HTTP redirect locations are unavailable");
            return false;
        }
    }

    if (code >= 300 && code < 400) {
        fail("HTTP redirects are unavailable");
        return false;
    }
    filesystem::SpaceInfo space{};
    if (code != 200 || !length_seen || unsupported) {
        fail("HTTP requires status 200 and an unencoded Content-Length body");
        return false;
    }
    if (filesystem::remove_if_exists(kStagingPath) != filesystem::MutationResult::succeeded ||
        !filesystem::space(space) || space.used_bytes > space.total_bytes ||
        expected_ > space.total_bytes - space.used_bytes ||
        filesystem::ensure_directory("/.newmota") != filesystem::MutationResult::succeeded ||
        filesystem::open_writer(kStagingPath, writer_) != filesystem::MutationResult::succeeded) {
        fail("Insufficient storage or staging file unavailable");
        return false;
    }
    state_ = State::body;
    if (expected_ == 0) {
        if (response.size() != separator + 4) {
            fail("Unexpected response body");
            return false;
        }
        state_ = State::checking_end;
        return true;
    }
    const auto offset = separator + 4;
    return offset == response.size() ||
           consume(reinterpret_cast<const std::uint8_t*>(response.data() + offset),
               response.size() - offset);
}

/** @brief Keeps the last outcome queryable after the active transport phases finish. */
const char* WgetService::state_name() const {
    switch (state_) {
    case State::idle:
        return result_;
    case State::preparing:
        return "preparing";
    case State::closing:
        return "closing-connection";
    case State::connecting:
        return "connecting";
    case State::sending:
        return "sending-request";
    case State::headers:
        return "receiving-headers";
    case State::body:
        return "downloading";
    case State::checking_end:
        return "checking-response-end";
    case State::draining:
        return cleanup_timed_out_ ? "cleanup-fault" : "closing-connection";
    }
    return "failed";
}

/** @brief Performs one nonblocking operation and bounds inactivity, total time, and cleanup. */
void WgetService::poll(std::uint32_t now, bool network_ready, bool restart_pending) {
    if (!active())
        return;
    polled_at_ = now;
    if (state_ == State::draining) {
        const auto native = stream_.state(stream_.context);
        if (native == platform::TcpStreamState::idle ||
            native == platform::TcpStreamState::unavailable) {
            state_ = State::idle;
        } else if (!cleanup_timed_out_ && now - cleanup_started_at_ >= 5000U) {
            // Native open stays busy until closure completes, even after this lease expires.
            cleanup_timed_out_ = true;
            result_ = "failed";
            error_ = "TCP cleanup timeout";
            AddLog(LogLevel::error, "WGET: failed: %s", error_);
            state_ = State::idle;
        }
        return;
    }
    if (restart_pending || now - started_at_ >= kTotalTimeout ||
        now - progress_at_ >= kIdleTimeout) {
        fail(restart_pending ? "Restart pending" : "HTTP timeout");
        return;
    }
    if (state_ == State::preparing) {
        // Allow the command response to leave before disconnecting shared MQTT.
        if (now - started_at_ < 1200U)
            return;
        stream_.close(stream_.context);
        state_ = State::closing;
        progress_at_ = now;
        return;
    }
    const auto native = stream_.state(stream_.context);
    if (state_ == State::closing) {
        if (now - progress_at_ >= 5000U) {
            fail("TCP close or network readiness timeout");
        } else if (native == platform::TcpStreamState::idle && network_ready) {
            const auto opened = stream_.open(stream_.context, buffers_->host.data(), port_, false);
            if (opened == platform::TcpStreamOpenResult::accepted) {
                state_ = State::connecting;
                progress_at_ = now;
            } else if (opened != platform::TcpStreamOpenResult::busy) {
                fail("TCP open failed");
            }
        } else if (native == platform::TcpStreamState::unavailable) {
            fail("TCP unavailable");
        }
        return;
    }
    if (state_ == State::connecting) {
        if (native == platform::TcpStreamState::connected) {
            state_ = State::sending;
            progress_at_ = now;
        } else if (native != platform::TcpStreamState::connecting) {
            fail("HTTP connection failed");
        }
        return;
    }
    if (state_ == State::sending) {
        const auto written = stream_.write(stream_.context,
            reinterpret_cast<const std::uint8_t*>(buffers_->request.data() + sent_),
            request_size_ - sent_);
        if (written.state == platform::TcpStreamIoState::progressed && written.size > 0 &&
            written.size <= request_size_ - sent_) {
            sent_ += written.size;
            progress_at_ = now;
            if (sent_ == request_size_)
                state_ = State::headers;
        } else if (written.state != platform::TcpStreamIoState::would_block) {
            fail("HTTP request failed");
        }
        return;
    }
    std::array<std::uint8_t, 512> body{};
    const bool reading_headers = state_ == State::headers;
    const auto capacity = reading_headers ? buffers_->headers.size() - header_size_ : body.size();
    if (capacity == 0) {
        fail("HTTP headers exceed 1024 bytes");
        return;
    }
    auto* target = reading_headers
                       ? reinterpret_cast<std::uint8_t*>(buffers_->headers.data() + header_size_)
                       : body.data();
    const auto read = stream_.read(stream_.context, target, capacity);
    if (read.state == platform::TcpStreamIoState::progressed && read.size > 0 &&
        read.size <= capacity) {
        progress_at_ = now;
        if (reading_headers) {
            header_size_ += read.size;
            if (std::string_view{buffers_->headers.data(), header_size_}.find("\r\n\r\n") !=
                std::string_view::npos)
                (void)parse_headers();
        } else {
            (void)consume(body.data(), read.size);
        }
    } else if (read.state == platform::TcpStreamIoState::closed && state_ == State::checking_end) {
        finish();
    } else if (read.state != platform::TcpStreamIoState::would_block) {
        fail("HTTP response ended early");
    }
}
} // namespace newmota
