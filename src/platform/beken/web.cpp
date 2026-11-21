/*
  web.cpp - Beken BDK bounded HTTP adapter for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota_features.h"
#include "newmota/platform/web.h"
#include "newmota/platform/web_http.h"
#if USE_WEB_HTTPS
#include "newmota/platform/web_tls_socket.h"
#endif

extern "C" {
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "net.h"
#include "rtos_pub.h"

// BkDriverRng.h needs the legacy OSStatus header first; OSStatus is int.
// BK7231N and BK7238 read the hardware TRNG here. bk_rand() is not used
// because it masks each word with RAND_MAX and loses the top bit.
int BkRandomNumberRead(void* inBuffer, int inByteCount);
}

#include <atomic>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace newmota::platform {
namespace {

// Beken's application HTTP path is intentionally single-connection.  The
// fixed mailbox serializes application work; idle sessions yield to waiting clients.
constexpr std::size_t kHeaderLimit = 2048;
constexpr std::size_t kReceiveCapacity = kHeaderLimit + WebRequest{}.body.size();
// An idle keep-alive client gives up the one-client task soon; a started request gets time for a
// lost Wi-Fi segment to be sent again, as Arduino's WebServer allows (HTTP_MAX_DATA_WAIT).
constexpr std::uint32_t kIdleReceiveTimeoutMilliseconds = 2000;
constexpr std::uint32_t kRequestReceiveTimeoutMilliseconds = 5000;
constexpr std::uint32_t kApplicationTimeoutMilliseconds = 6000;
constexpr std::uint16_t kHttpPort = 80;
constexpr std::uint16_t kHttpsPort = 443;
constexpr std::uint32_t kServerStackSize = USE_WEB_HTTPS ? 12288U : 4096U;
constexpr int kInvalidSocket = -1;

enum class Phase : std::uint8_t { idle, pending, processing, ready };

std::atomic<Phase> phase{Phase::idle};
std::atomic<bool> cancelled{false};
int listener = kInvalidSocket;
beken_thread_t server_thread{};
WebExchange exchange;
std::array<char, kReceiveCapacity> receive_buffer{};
std::size_t receive_size{};
bool request_close{true};

/** @brief Tests a wrapping millisecond deadline. */
[[nodiscard]] bool deadline_reached(const std::uint32_t now, const std::uint32_t deadline) {
    return static_cast<std::int32_t>(now - deadline) >= 0;
}

using web_http::ParseResult;

std::size_t header_end() {
    return web_http::header_end(receive_buffer.data(), receive_size);
}

ParseResult parse_request(const std::size_t headers_end, unsigned int& rejection_status) {
    const auto result = web_http::parse_request(receive_buffer.data(), receive_size,
        exchange.request, request_close, headers_end, rejection_status);
    if (result == ParseResult::ready)
        exchange.request.secure = USE_WEB_HTTPS != 0;
    return result;
}

/** @brief Sends all bytes or reports a disconnected client. */
bool send_all(const int client, const void* data, const std::size_t size) {
    const auto* bytes = static_cast<const char*>(data);
    std::size_t offset = 0;
    const auto deadline = rtos_get_time() + kApplicationTimeoutMilliseconds;
    while (offset < size) {
        if (deadline_reached(rtos_get_time(), deadline))
            return false;
#if USE_WEB_HTTPS
        const auto sent = web_tls_socket::write_tls(bytes + offset, size - offset);
#else
        const auto sent = send(client, bytes + offset, size - offset, 0);
#endif
#if USE_WEB_HTTPS
        if (sent == web_tls_socket::retry_read || sent == web_tls_socket::retry_write) {
            web_tls_socket::wait_retry(sent);
            continue;
        }
#endif
        if (sent <= 0)
            return false;
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

/** @brief Sends one complete fixed-length response, reporting partial writes. */
bool send_response(const int client, const WebResponseView& response) {
    std::array<char, 768> header{};
    const auto* content_type =
        response.content_type != nullptr ? response.content_type : "application/octet-stream";
    int written = std::snprintf(header.data(), header.size(),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lu\r\n"
        "Connection: %s\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; "
        "style-src 'unsafe-inline'; connect-src 'self'; base-uri 'none'; "
        "form-action 'self'; frame-ancestors 'none'\r\n",
        web_status_line(response.status), content_type, static_cast<unsigned long>(response.size),
        request_close ? "close" : "keep-alive");
    if (written < 0 || static_cast<std::size_t>(written) >= header.size())
        return false;
    auto header_size = static_cast<std::size_t>(written);
    if (response.content_encoding != nullptr) {
        written = std::snprintf(header.data() + header_size, header.size() - header_size,
            "Content-Encoding: %s\r\n", response.content_encoding);
        if (written < 0 || static_cast<std::size_t>(written) >= header.size() - header_size)
            return false;
        header_size += static_cast<std::size_t>(written);
    }
    if (response.cookie[0] != '\0') {
        written = std::snprintf(header.data() + header_size, header.size() - header_size,
            "Set-Cookie: %s\r\n", response.cookie);
        if (written < 0 || static_cast<std::size_t>(written) >= header.size() - header_size)
            return false;
        header_size += static_cast<std::size_t>(written);
    }
    if (header_size + 2 >= header.size())
        return false;
    header[header_size++] = '\r';
    header[header_size++] = '\n';
    if (!send_all(client, header.data(), header_size))
        return false;
    return send_all(client, response.body, response.size);
}

/** @brief Sends a short parser rejection without entering the app mailbox. */
void reject(const int client, const unsigned int status, const char* message) {
    request_close = true;
    std::array<char, kWebRejectionCapacity> body{};
    const auto written = std::snprintf(body.data(), body.size(), "{\"error\":\"%s\"}", message);
    WebResponseView response{};
    response.status = status;
    response.body = body.data();
    if (written > 0 && static_cast<std::size_t>(written) < body.size())
        response.size = static_cast<std::size_t>(written);
    send_response(client, response);
}

/** @brief Reads one bounded request while preserving subsequent stream bytes. */
bool receive_request(const int client, unsigned int& rejection_status) {
    bool receiving = receive_size != 0;
    auto deadline = rtos_get_time() + (receiving ? kRequestReceiveTimeoutMilliseconds
                                                 : kIdleReceiveTimeoutMilliseconds);
    for (;;) {
        const auto end = header_end();
        if (end != 0) {
            const auto parsed = parse_request(end, rejection_status);
            if (parsed == ParseResult::ready)
                return true;
            if (parsed == ParseResult::rejected)
                return false;
        } else if (receive_size >= kHeaderLimit) {
            rejection_status = 431;
            return false;
        }
        if (receive_size == receive_buffer.size()) {
            rejection_status = 431;
            return false;
        }
        if (deadline_reached(rtos_get_time(), deadline)) {
            rejection_status = receiving ? 408 : 0;
            return false;
        }
#if USE_WEB_HTTPS
        const auto received = web_tls_socket::read_tls(
            receive_buffer.data() + receive_size, receive_buffer.size() - receive_size);
        if (received == web_tls_socket::retry_read || received == web_tls_socket::retry_write) {
            web_tls_socket::wait_retry(received);
            continue;
        }
#else
        const auto received = recv(
            client, receive_buffer.data() + receive_size, receive_buffer.size() - receive_size, 0);
        // The socket timeout is one wait step; the deadline above ends the request.
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            continue;
#endif
        if (received <= 0) {
            rejection_status = receiving ? 408 : 0;
            return false;
        }
        if (!receiving) {
            receiving = true;
            deadline = rtos_get_time() + kRequestReceiveTimeoutMilliseconds;
        }
        receive_size += static_cast<std::size_t>(received);
    }
}

/** @brief Handles one accepted client in the dedicated Beken HTTP task. */
void handle_client(const int client) {
    // This lwIP port uses the POSIX timeval form for socket timeouts (the
    // non-standard integer form is disabled in its lwipopts.h).
    const timeval timeout{2, 0};
    (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#if USE_WEB_HTTPS
    if (!web_tls_socket::accept_client(client))
        return;
#endif
    // Requests wait in short steps so receive_request() applies its own deadlines.
    const timeval receive_step{0, 500000};
    (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &receive_step, sizeof(receive_step));

    // A timed-out application exchange remains occupied until the application
    // loop observes cancellation.  Never let a later socket overwrite it.
    if (cancelled.load() && phase.load() == Phase::ready)
        phase.store(Phase::idle);
    if (phase.load() != Phase::idle) {
        reject(client, 503, "Service unavailable");
        return;
    }

    receive_size = 0;
    for (;;) {
        unsigned int rejection_status = 400;
        if (!receive_request(client, rejection_status)) {
            if (rejection_status != 0)
                reject(client, rejection_status,
                    rejection_status == 413   ? "Content too large"
                    : rejection_status == 414 ? "URI too long"
                    : rejection_status == 431 ? "Header too large"
                                              : "Bad request");
            return;
        }

        cancelled.store(false);
        phase.store(Phase::pending);
        const auto deadline = rtos_get_time() + kApplicationTimeoutMilliseconds;
        while (phase.load() != Phase::ready && !deadline_reached(rtos_get_time(), deadline)) {
            rtos_delay_milliseconds(1);
        }
        if (phase.load() != Phase::ready || cancelled.load()) {
            // Keep the mailbox occupied until web_request() or web_complete()
            // drains the timed-out exchange.  This mirrors the ownership contract
            // used by the other native adapters and prevents a late completion
            // from corrupting a newer request.
            cancelled.store(true);
            reject(client, 504, "Application timeout");
            return;
        }
        const bool sent = send_response(client, view(exchange.response));
        exchange.request.reset();
        exchange.response.reset();
        phase.store(Phase::idle);
        if (!sent || request_close)
            return;
        // Allow a waiting client to reclaim the single transport at exchange boundaries.
        fd_set pending;
        FD_ZERO(&pending);
        FD_SET(listener, &pending);
        timeval immediate{0, 0};
        if (select(listener + 1, &pending, nullptr, nullptr, &immediate) > 0)
            return;
    }
}

/** @brief Runs the one-client Beken socket loop without blocking Newmota. */
void server_task(void*) {
    for (;;) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        timeval timeout{1, 0};
        if (select(listener + 1, &readable, nullptr, nullptr, &timeout) <= 0)
            continue;
        if (!FD_ISSET(listener, &readable))
            continue;
        sockaddr_storage address{};
        socklen_t length = sizeof(address);
        const auto client = accept(listener, reinterpret_cast<sockaddr*>(&address), &length);
        if (client < 0)
            continue;
        handle_client(client);
#if USE_WEB_HTTPS
        web_tls_socket::close_tls();
#endif
        (void)close(client);
    }
}

} // namespace

/** @brief Starts one Beken socket HTTP listener on any configured interface. */
bool web_start(const WebTlsIdentity* identity) {
#if USE_WEB_HTTPS
    if (identity == nullptr || !web_tls_socket::start_server(*identity))
        return false;
#else
    (void)identity;
#endif
    if (listener != kInvalidSocket)
        return true;
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener < 0) {
        listener = kInvalidSocket;
        return false;
    }
    int reuse = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(USE_WEB_HTTPS ? kHttpsPort : kHttpPort);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
        (void)close(listener);
        listener = kInvalidSocket;
        return false;
    }
    if (rtos_create_thread(&server_thread, THD_APPLICATION_PRIORITY, "newmota-http", server_task,
            kServerStackSize, nullptr) != kNoErr) {
        (void)close(listener);
        listener = kInvalidSocket;
        return false;
    }
    return true;
}

/** @brief Acquires application ownership after the HTTP task publishes a request. */
WebExchange* web_request() {
    if (phase.load() != Phase::pending)
        return nullptr;
    if (cancelled.load()) {
        phase.store(Phase::idle);
        exchange.request.body.fill(0);
        return nullptr;
    }
    phase.store(Phase::processing);
    auto& output = exchange.response;
    output.reset();
    return &exchange;
}

/** @brief Publishes the response for the Beken HTTP task without network I/O. */
void web_complete() {
    if (phase.load() == Phase::processing)
        phase.store(Phase::ready);
}

/** @brief Uses the Beken hardware RNG independently of station association. */
bool web_random(void* destination, const std::size_t size) {
    if (destination == nullptr || size > static_cast<std::size_t>(INT32_MAX))
        return false;
    return BkRandomNumberRead(destination, static_cast<int>(size)) == 0;
}

} // namespace newmota::platform
