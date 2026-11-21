/*
  web.h - bounded HTTP backend boundary for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <array>
#include <cstddef>

#ifndef NEWMOTA_WEB_REQUEST_BODY_SIZE
#define NEWMOTA_WEB_REQUEST_BODY_SIZE 2048
#endif

namespace newmota::platform {

enum class WebMethod { get, post };

struct WebRequest final {
    WebMethod method{};
    bool secure{};
    std::array<char, 96> path{}, host{}, accept_encoding{};
    std::array<char, 128> origin{}, cookie{};
    std::array<char, 48> csrf{}, content_type{}, content_range{};
    std::array<char, 24> fetch_site{};
    // The generated role bound allows bounded Berry source in application
    // images and keeps Safeboot recovery requests smaller.
    std::array<char, NEWMOTA_WEB_REQUEST_BODY_SIZE> body{};
    std::size_t body_size{};

    /** @brief Clears the mailbox request in place without a large stack temporary. */
    void reset() noexcept {
        method = WebMethod::get;
        secure = false;
        path.fill('\0');
        host.fill('\0');
        accept_encoding.fill('\0');
        origin.fill('\0');
        cookie.fill('\0');
        csrf.fill('\0');
        content_type.fill('\0');
        content_range.fill('\0');
        fetch_site.fill('\0');
        body.fill('\0');
        body_size = 0;
    }
};

struct WebResponse final {
    /** @brief Keeps global response buffers in BSS through runtime initialization. */
    WebResponse();
    unsigned int status{200};
    const char* content_type{"application/json"};
    const char* content_encoding{};
    // Non-null only for immutable firmware-lifetime assets; never caller stack data.
    const char* asset{};
    std::array<char, 128> cookie{};
    std::array<char, 4096> body{};
    std::size_t size{};

    /** @brief Clears response storage in place without a large stack temporary. */
    void reset() noexcept {
        status = 200;
        content_type = "application/json";
        content_encoding = nullptr;
        asset = nullptr;
        cookie.fill('\0');
        body.fill('\0');
        size = 0;
    }
};

/** @brief What a native server sends: a WebResponse, or a short body it owns itself. */
struct WebResponseView final {
    unsigned int status{200};
    const char* content_type{"application/json"};
    const char* content_encoding{};
    const char* cookie{""}; ///< "" sends no Set-Cookie.
    const char* body{};
    std::size_t size{};
};

/** @brief Views an application response; the response must outlive the send. */
[[nodiscard]] inline WebResponseView view(const WebResponse& response) {
    return {response.status, response.content_type, response.content_encoding,
        response.cookie.data(), response.asset != nullptr ? response.asset : response.body.data(),
        response.size};
}

/** @brief Body capacity of a native parser or mailbox rejection. */
inline constexpr std::size_t kWebRejectionCapacity = 96;

/** @brief Returns the status line ("404 Not Found") for every status the service emits. */
[[nodiscard]] const char* web_status_line(unsigned int status);

struct WebExchange final {
    WebRequest request{};
    WebResponse response{};
};

/** @brief Bounded persistent server identity supplied to an HTTPS adapter. */
struct WebTlsIdentity final {
    std::array<char, 2048> certificate_pem{};
    std::array<char, 1024> private_key_pem{};
    std::size_t certificate_size{};
    std::size_t private_key_size{};
};

/** @brief Starts the selected backend after Wi-Fi initialization; retries are safe. */
[[nodiscard]] bool web_start(const WebTlsIdentity* identity = nullptr);
/**
 * @brief Takes one owned request without waiting; called only by the application loop.
 * The exchange remains exclusively application-owned until web_complete().
 * The backend owns network I/O, bounded receive/send deadlines and disconnected clients.
 */
[[nodiscard]] WebExchange* web_request();
/**
 * @brief Publishes the completed response without sending or waiting on a socket.
 * The application must no longer access the exchange after this call.
 */
void web_complete();
/** @brief Fills session entropy from a qualified native source, or fails closed. */
[[nodiscard]] bool web_random(void* destination, std::size_t size);

} // namespace newmota::platform
