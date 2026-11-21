/*
  web.cpp - ESP-IDF HTTP server adapter for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota_features.h"
#include "newmota/platform/web.h"
#include "esp_http_server.h"
#include "esp_httpd_priv.h"
#if USE_WEB_HTTPS
#include "esp_https_server.h"
#endif
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <strings.h>

namespace newmota::platform {
namespace {

enum class Phase { idle, pending, processing, ready };
std::atomic<Phase> phase{Phase::idle};
std::atomic<bool> cancelled{false};
WebExchange exchange;
httpd_handle_t server{};

/** @brief Copies a complete selected header, rejecting truncation. */
template <std::size_t N>
bool header(httpd_req_t* request, const char* name, std::array<char, N>& value) {
    const auto size = httpd_req_get_hdr_value_len(request, name);
    if (size == 0)
        return true;
    return size < N && httpd_req_get_hdr_value_str(request, name, value.data(), N) == ESP_OK;
}

/** @brief Finds the case-insensitive close token in one Connection field. */
bool contains_close(std::string_view value) {
    while (!value.empty()) {
        const auto comma = value.find(',');
        auto token = value.substr(0, comma);
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
            token.remove_prefix(1);
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
            token.remove_suffix(1);
        if (token.size() == 5) {
            bool matches = true;
            constexpr std::string_view expected{"close"};
            for (std::size_t index = 0; index < token.size(); ++index) {
                auto character = token[index];
                if (character >= 'A' && character <= 'Z')
                    character += 'a' - 'A';
                matches = matches && character == expected[index];
            }
            if (matches)
                return true;
        }
        if (comma == std::string_view::npos)
            break;
        value.remove_prefix(comma + 1);
    }
    return false;
}

/** @brief Scans every SDK-parsed Connection field so close always wins. */
bool close_requested(httpd_req_t* request, bool& close) {
    close = false;
    // The public header lookup returns only the first matching field. Use the pinned
    // SDK's parsed storage, whose header lines are separated by NUL characters.
    const auto* auxiliary = static_cast<const httpd_req_aux*>(request->aux);
    if (auxiliary->req_hdrs_count == 0)
        return true;
    if (!auxiliary->scratch)
        return false;
    std::string_view headers{auxiliary->scratch, auxiliary->scratch_cur_size};
    for (unsigned int index = 0; index < auxiliary->req_hdrs_count; ++index) {
        while (!headers.empty() && headers.front() == '\0')
            headers.remove_prefix(1);
        const auto end = headers.find('\0');
        if (end == std::string_view::npos)
            return false;
        const auto line = headers.substr(0, end);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos)
            return false;
        constexpr std::string_view name{"Connection"};
        if (colon == name.size() && strncasecmp(line.data(), name.data(), name.size()) == 0) {
            auto value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ')
                value.remove_prefix(1);
            if (value.size() >= 128)
                return false;
            close = close || contains_close(value);
        }
        headers.remove_prefix(end + 1);
    }
    return true;
}

/** @brief Closes rejected requests so unread bodies cannot become another request. */
esp_err_t reject(httpd_req_t* request, const char* status) {
    (void)httpd_resp_set_status(request, status);
    (void)httpd_resp_set_hdr(request, "Connection", "close");
    (void)httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    (void)httpd_resp_send(request, nullptr, 0);
    return ESP_FAIL;
}

/**
 * @brief Transfers a bounded request to the application and sends its completed reply.
 * Only the SDK server task waits on the application or network. It serializes mailbox
 * producers; late application completion after a timeout cannot reach a newer request.
 */
esp_err_t handle(httpd_req_t* request) {
    if (cancelled.load() && phase.load() == Phase::ready)
        phase.store(Phase::idle);
    if (phase.load() != Phase::idle)
        return reject(request, "503 Service Unavailable");
    if (request->content_len > exchange.request.body.size())
        return reject(request, "413 Content Too Large");
    bool close = false;
    if (!close_requested(request, close))
        return reject(request, "431 Request Header Fields Too Large");
    if (httpd_req_get_hdr_value_len(request, "Transfer-Encoding") != 0)
        return reject(request, "400 Bad Request");
    exchange.request.reset();
    auto& input = exchange.request;
    input.method = request->method == HTTP_POST ? WebMethod::post : WebMethod::get;
    input.secure = USE_WEB_HTTPS != 0;
    if (std::strlen(request->uri) >= input.path.size())
        return reject(request, "414 URI Too Long");
    std::strcpy(input.path.data(), request->uri);
    if (!header(request, "Host", input.host) || !header(request, "Origin", input.origin) ||
        !header(request, "Cookie", input.cookie) ||
        !header(request, "X-Newmota-CSRF", input.csrf) ||
        !header(request, "Content-Type", input.content_type) ||
        !header(request, "Content-Range", input.content_range) ||
        !header(request, "Sec-Fetch-Site", input.fetch_site) ||
        !header(request, "Accept-Encoding", input.accept_encoding))
        return reject(request, "431 Request Header Fields Too Large");
    // GET bodies are not part of this API; do not let the SDK drain arbitrary input.
    if (input.method == WebMethod::get && request->content_len != 0)
        return reject(request, "400 Bad Request");
    // Time for a lost Wi-Fi segment to be sent again, as Arduino's WebServer allows
    // (HTTP_MAX_DATA_WAIT); one recv_wait_timeout is only a wait step.
    const auto receive_start = xTaskGetTickCount();
    while (input.body_size < request->content_len) {
        if (xTaskGetTickCount() - receive_start > pdMS_TO_TICKS(5000))
            return reject(request, "408 Request Timeout");
        const auto received = httpd_req_recv(
            request, input.body.data() + input.body_size, request->content_len - input.body_size);
        if (received == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (received <= 0)
            return reject(request, "408 Request Timeout");
        input.body_size += static_cast<std::size_t>(received);
    }
    cancelled.store(false);
    phase.store(Phase::pending);
    const auto start = xTaskGetTickCount();
    while (phase.load() != Phase::ready) {
        if (xTaskGetTickCount() - start > pdMS_TO_TICKS(6000)) {
            cancelled.store(true);
            return reject(request, "504 Gateway Timeout");
        }
        vTaskDelay(1);
    }
    const auto& output = exchange.response;
    (void)httpd_resp_set_status(request, web_status_line(output.status));
    if (close)
        (void)httpd_resp_set_hdr(request, "Connection", "close");
    (void)httpd_resp_set_type(request, output.content_type);
    (void)httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    (void)httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    (void)httpd_resp_set_hdr(request, "X-Frame-Options", "DENY");
    (void)httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    (void)httpd_resp_set_hdr(request, "Content-Security-Policy",
        "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
        "connect-src 'self'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'"
#if USE_WEBCAM || USE_CSI_WEBCAM
        "; frame-src *:81" // the Home camera card embeds the port-81 stream page
#endif
    );
    if (output.content_encoding != nullptr)
        (void)httpd_resp_set_hdr(request, "Content-Encoding", output.content_encoding);
    if (output.cookie[0] != '\0')
        (void)httpd_resp_set_hdr(request, "Set-Cookie", output.cookie.data());
    const auto result =
        httpd_resp_send(request, output.asset != nullptr ? output.asset : output.body.data(),
            static_cast<ssize_t>(output.size));
    input.body.fill(0);
    phase.store(Phase::idle);
    // Returning failure after the complete response asks HTTPD to retire the session.
    return close && result == ESP_OK ? ESP_FAIL : result;
}

} // namespace

/** @brief Starts the selected bounded HTTP/1.1 server without WebSockets. */
bool web_start(const WebTlsIdentity* identity) {
    if (server != nullptr)
        return true;
#if USE_WEB_HTTPS
    if (identity == nullptr || identity->certificate_size == 0U || identity->private_key_size == 0U)
        return false;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
#pragma GCC diagnostic pop
    config.use_secure_element = false;
    config.httpd.max_open_sockets = 2;
    config.httpd.max_uri_handlers = 2;
    config.httpd.max_resp_headers = 10;
    config.httpd.max_req_hdr_len = 2048;
    config.httpd.lru_purge_enable = true;
    config.httpd.recv_wait_timeout = 1;
    config.httpd.send_wait_timeout = 1;
    config.httpd.uri_match_fn = httpd_uri_match_wildcard;
    config.tls_handshake_timeout_ms = 10000;
    config.servercert = reinterpret_cast<const std::uint8_t*>(identity->certificate_pem.data());
    config.servercert_len = identity->certificate_size + 1U;
    config.prvtkey_pem = reinterpret_cast<const std::uint8_t*>(identity->private_key_pem.data());
    config.prvtkey_len = identity->private_key_size + 1U;
    if (httpd_ssl_start(&server, &config) != ESP_OK)
        return false;
#else
    (void)identity;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 4;
    config.max_uri_handlers = 2;
    config.max_resp_headers = 10;
    config.max_req_hdr_len = 2048;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 1;
    config.send_wait_timeout = 1;
    config.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&server, &config) != ESP_OK)
        return false;
#endif
    httpd_uri_t route{};
    route.uri = "/*";
    route.handler = handle;
    route.method = HTTP_GET;
    const auto get_result = httpd_register_uri_handler(server, &route);
    route.method = HTTP_POST;
    if (get_result != ESP_OK || httpd_register_uri_handler(server, &route) != ESP_OK) {
        (void)httpd_stop(server);
        server = nullptr;
        return false;
    }
    return true;
}

/** @brief Acquires application ownership after the SDK task publishes the request. */
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

/** @brief Releases the response to the SDK task without doing network work. */
void web_complete() {
    phase.store(Phase::ready);
}

/** @brief Uses ESP's hardware RNG independently of station association. */
bool web_random(void* destination, const std::size_t size) {
    if (destination == nullptr)
        return false;
    esp_fill_random(destination, size);
    return true;
}

} // namespace newmota::platform
