/*
  web.cpp - Realtek AmebaD HTTPD adapter for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota_features.h"
#include "newmota/platform/web.h"

extern "C" {
#include "FreeRTOS.h"
#include "httpd/httpd.h"
#include "osdep_service.h"
#include "task.h"
#include "wifi_conf.h"
#include "wifi_constants.h"
}

#include "newmota/platform/web_http.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace newmota::platform {
namespace {

// Ameba's vendor HTTPD can block when one write exceeds its internal send
// buffer (the SDK example uses 2 KiB chunks for files larger than 6 KiB).
constexpr std::size_t kResponseChunkSize = 2000;

enum class Phase { idle, pending, processing, ready };
std::atomic<Phase> phase{Phase::idle};
std::atomic<bool> cancelled{false};
WebExchange exchange;
bool started{};
#if USE_WEB_HTTPS
bool tls_identity_ready{};
#endif

/** @brief Copies a native field into a fixed request slot and rejects truncation. */
template <std::size_t N>
bool copy_field(const uint8_t* source, const std::size_t size, std::array<char, N>& destination) {
    if (source == nullptr || size >= N)
        return false;
    if (size != 0)
        std::memcpy(destination.data(), source, size);
    destination[size] = '\0';
    return true;
}

/** @brief Copies one optional HTTP header and releases the SDK allocation. */
template <std::size_t N>
bool header(httpd_conn* connection, const char* name, std::array<char, N>& destination) {
    char* value = nullptr;
    if (httpd_request_get_header_field(connection, const_cast<char*>(name), &value) != 0)
        return true;
    if (value == nullptr)
        return false;
    const auto size = std::strlen(value);
    const bool result = copy_field(reinterpret_cast<const uint8_t*>(value), size, destination);
    httpd_free(value);
    return result;
}

using web_http::ascii_iequals;
using web_http::decimal;
using web_http::trim_ows;

/** @brief Verifies framing and all Connection fields before the vendor discards headers. */
bool request_framing(httpd_conn* connection, bool& close) {
    const auto& request = connection->request;
    if (request.header == nullptr || request.version == nullptr)
        return false;
    const std::string_view version{
        reinterpret_cast<const char*>(request.version), request.version_len};
    close = version != "HTTP/1.1";
    const std::string_view headers{
        reinterpret_cast<const char*>(request.header), request.header_len};
    auto start = headers.find("\r\n");
    if (start == std::string_view::npos)
        return false;
    start += 2;
    bool length_seen = false;
    std::size_t content_length = 0;
    while (start < headers.size()) {
        const auto end = headers.find("\r\n", start);
        if (end == std::string_view::npos)
            return false;
        if (end == start)
            return content_length == request.content_len;
        const auto line = headers.substr(start, end - start);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0)
            return false;
        const auto name = line.substr(0, colon);
        for (const auto character : name) {
            const bool token =
                (character >= '0' && character <= '9') || (character >= 'A' && character <= 'Z') ||
                (character >= 'a' && character <= 'z') ||
                std::string_view{"!#$%&'*+-.^_`|~"}.find(character) != std::string_view::npos;
            if (!token) {
                return false;
            }
        }
        auto value = trim_ows(line.substr(colon + 1));
        if (ascii_iequals(name, "Transfer-Encoding"))
            return false;
        if (ascii_iequals(name, "Content-Length")) {
            if (length_seen || !decimal(value, content_length))
                return false;
            length_seen = true;
        }
        if (ascii_iequals(name, "Connection")) {
            while (!value.empty()) {
                const auto comma = value.find(',');
                if (ascii_iequals(trim_ows(value.substr(0, comma)), "close"))
                    close = true;
                if (comma == std::string_view::npos)
                    break;
                value.remove_prefix(comma + 1);
            }
        }
        start = end + 2;
    }
    return content_length == request.content_len;
}

/** @brief Writes a fully framed response and retains only successfully sent sessions. */
void send_response(
    httpd_conn* connection, const WebResponseView& response, const bool close = true) {
    bool success = httpd_response_write_header_start(connection,
                       const_cast<char*>(web_status_line(response.status)),
                       const_cast<char*>(response.content_type), response.size) == 0;
    // These are the exact fixed-length fields produced by header_start in the pinned SDK.
    std::size_t expected_header_size = 9 + std::strlen(web_status_line(response.status)) + 2;
    if (response.content_type != nullptr)
        expected_header_size += 14 + std::strlen(response.content_type) + 2;
    if (response.size != 0) {
        char length[24]{};
        const auto digits =
            std::snprintf(length, sizeof(length), "%lu", static_cast<unsigned long>(response.size));
        if (digits <= 0 || static_cast<std::size_t>(digits) >= sizeof(length))
            success = false;
        else
            expected_header_size += 16 + static_cast<std::size_t>(digits) + 2;
    }
    const auto field = [&](const char* name, const char* value) {
        if (success) {
            success = httpd_response_write_header(
                          connection, const_cast<char*>(name), const_cast<char*>(value)) == 0;
            if (success)
                expected_header_size += std::strlen(name) + 2 + std::strlen(value) + 2;
        }
    };
    if (response.size == 0)
        field("Content-Length", "0");
    field("Connection", close ? "close" : "keep-alive");
    field("Cache-Control", "no-store");
    field("X-Content-Type-Options", "nosniff");
    field("X-Frame-Options", "DENY");
    field("Referrer-Policy", "no-referrer");
    field("Content-Security-Policy",
        "default-src 'none'; script-src 'unsafe-inline'; "
        "style-src 'unsafe-inline'; connect-src 'self'; base-uri 'none'; "
        "form-action 'self'; frame-ancestors 'none'");
    if (response.content_encoding != nullptr)
        field("Content-Encoding", response.content_encoding);
    if (response.cookie[0] != '\0')
        field("Set-Cookie", response.cookie);
    if (success && connection->response_header != nullptr) {
        // header_finish appends CRLF and returns the actual bytes written.
        success = httpd_response_write_header_finish(connection) ==
                  static_cast<int>(expected_header_size + 2);
    } else {
        success = false;
    }
    const auto* body = reinterpret_cast<const uint8_t*>(response.body);
    std::size_t offset = 0;
    const auto start = xTaskGetTickCount();
    while (success && offset < response.size) {
        if (xTaskGetTickCount() - start > pdMS_TO_TICKS(6000)) {
            success = false;
            break;
        }
        const auto remaining = response.size - offset;
        const auto chunk = remaining < kResponseChunkSize ? remaining : kResponseChunkSize;
        const auto written =
            httpd_response_write_data(connection, const_cast<uint8_t*>(body + offset), chunk);
        if (written <= 0 || static_cast<std::size_t>(written) > chunk) {
            success = false;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (close || !success)
        httpd_conn_close(connection);
}

/** @brief Rejects a request before it reaches the application mailbox. */
void reject(httpd_conn* connection, const unsigned int status, const char* message) {
    std::array<char, kWebRejectionCapacity> body{};
    const auto written = std::snprintf(body.data(), body.size(), "{\"error\":\"%s\"}", message);
    WebResponseView response{};
    response.status = status;
    response.body = body.data();
    if (written > 0 && static_cast<std::size_t>(written) < body.size())
        response.size = static_cast<std::size_t>(written);
    send_response(connection, response);
}

/** @brief Copies the parsed request and waits for the application-owned response. */
void callback(httpd_conn* connection) {
    // A timed-out callback has already closed its socket; reclaim the mailbox
    // after the application observes the cancellation before accepting another one.
    if (cancelled.load() && phase.load() == Phase::ready)
        phase.store(Phase::idle);
    if (phase.load() != Phase::idle) {
        reject(connection, 503, "Service unavailable");
        return;
    }
    bool close = true;
    if (!request_framing(connection, close)) {
        reject(connection, 400, "Invalid framing");
        return;
    }
    auto& input = exchange.request;
    input = {};
    if (!copy_field(connection->request.path, connection->request.path_len, input.path) ||
        !copy_field(connection->request.host, connection->request.host_len, input.host)) {
        reject(connection, connection->request.path_len >= input.path.size() ? 414 : 431,
            connection->request.path_len >= input.path.size() ? "URI too long"
                                                              : "Header too large");
        return;
    }
    if (httpd_request_is_method(connection, const_cast<char*>("GET"))) {
        input.method = WebMethod::get;
    } else if (httpd_request_is_method(connection, const_cast<char*>("POST"))) {
        input.method = WebMethod::post;
    } else {
        reject(connection, 400, "Unsupported method");
        return;
    }
    if (!header(connection, "Origin", input.origin) ||
        !header(connection, "Cookie", input.cookie) ||
        !header(connection, "X-Newmota-CSRF", input.csrf) ||
        !header(connection, "Content-Type", input.content_type) ||
        !header(connection, "Content-Range", input.content_range) ||
        !header(connection, "Sec-Fetch-Site", input.fetch_site) ||
        !header(connection, "Accept-Encoding", input.accept_encoding)) {
        reject(connection, 431, "Header too large");
        return;
    }
    const auto content_length = connection->request.content_len;
    if (content_length > input.body.size()) {
        reject(connection, 413, "Content too large");
        return;
    }
    if (input.method == WebMethod::get && content_length != 0) {
        reject(connection, 400, "GET body is not supported");
        return;
    }
    input.body_size = 0;
    // Time for a lost Wi-Fi segment to be sent again, as Arduino's WebServer allows
    // (HTTP_MAX_DATA_WAIT). One read waits at most the 2 s idle timeout; a negative result is
    // that timeout or a broken connection, and the deadline ends either.
    const auto receive_start = xTaskGetTickCount();
    while (input.body_size < content_length) {
        if (xTaskGetTickCount() - receive_start > pdMS_TO_TICKS(5000)) {
            reject(connection, 408, "Request timeout");
            return;
        }
        const auto remaining = content_length - input.body_size;
        const auto received = httpd_request_read_data(
            connection, reinterpret_cast<uint8_t*>(input.body.data() + input.body_size), remaining);
        if (received < 0) {
            vTaskDelay(1);
            continue;
        }
        if (received == 0 || static_cast<std::size_t>(received) > remaining) {
            reject(connection, 408, "Request timeout");
            return;
        }
        input.body_size += static_cast<std::size_t>(received);
    }
    input.secure = connection->tls != nullptr;
    cancelled.store(false);
    phase.store(Phase::pending);
    const auto start = xTaskGetTickCount();
    while (phase.load() != Phase::ready) {
        if (xTaskGetTickCount() - start > pdMS_TO_TICKS(6000)) {
            cancelled.store(true);
            reject(connection, 504, "Application timeout");
            return;
        }
        vTaskDelay(1);
    }
    send_response(connection, view(exchange.response), close);
    exchange.request.body.fill(0);
    exchange.response.body.fill(0);
    phase.store(Phase::idle);
}

// The vendor matches registered paths exactly, then falls back to a page named "*".
// The service owns routing, including its 404s and the /api/widgets/<id> style paths.
// Keep this in static storage because the SDK retains the pointer for the server lifetime.
char kCatchAllRoute[] = "*";

} // namespace

/** @brief Starts Realtek's bounded single-thread HTTP or HTTPS server. */
bool web_start(const WebTlsIdentity* identity) {
    if (started && httpd_is_running() != 0)
        return true;
#if USE_WEB_HTTPS
    if (!tls_identity_ready) {
        if (identity == nullptr || identity->certificate_size == 0U ||
            identity->private_key_size == 0U ||
            httpd_setup_cert(identity->certificate_pem.data(), identity->private_key_pem.data(),
                identity->certificate_pem.data()) != 0)
            return false;
        tls_identity_ready = true;
    }
#else
    (void)identity;
#endif
    started = false;
    httpd_clear_page_callbacks();
    httpd_setup_debug(HTTPD_DEBUG_OFF);
    httpd_setup_idle_timeout(2);
    httpd_set_header_size(2048);
    if (httpd_reg_page_callback(kCatchAllRoute, callback) != 0)
        return false;
    if (httpd_start(USE_WEB_HTTPS ? 443 : 80, USE_WEB_HTTPS ? 1 : 2, USE_WEB_HTTPS ? 12288 : 4096,
            HTTPD_THREAD_SINGLE, USE_WEB_HTTPS ? HTTPD_SECURE_TLS : HTTPD_SECURE_NONE) != 0) {
        httpd_clear_page_callbacks();
        return false;
    }
    started = true;
    return true;
}

/** @brief Acquires application ownership after the SDK task publishes a request. */
WebExchange* web_request() {
    if (phase.load() != Phase::pending)
        return nullptr;
    if (cancelled.load()) {
        exchange.request.body.fill(0);
        phase.store(Phase::ready);
        return nullptr;
    }
    phase.store(Phase::processing);
    auto& output = exchange.response;
    output.reset();
    return &exchange;
}

/** @brief Releases the response to the SDK HTTPD task without network work here. */
void web_complete() {
    phase.store(Phase::ready);
}

/** @brief Uses Realtek's hardware random source for either Wi-Fi interface. */
bool web_random(void* destination, const std::size_t size) {
    if (destination == nullptr)
        return false;
    return rtw_get_random_bytes(destination, static_cast<u32>(size)) == 0;
}

} // namespace newmota::platform
