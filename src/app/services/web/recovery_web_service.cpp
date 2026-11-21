/*
  recovery_web_service.cpp - compact Safeboot browser recovery service

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota_features.h"
#include "services/web/recovery_web_service.h"

#include "newmota/log.h"
#include "newmota/platform/network.h"
#include "newmota/platform/platform.h"
#include "newmota/platform/web.h"
#include "services/settings/settings.h"
#include "services/web/update_web_service.h"
#include "support/text.h"
#include "recovery_web_assets.h"

#include <cstdio>
#include <cstring>
#include <string_view>

namespace newmota {
namespace {

/** @brief Compares credentials and tokens without an early mismatch exit. */
bool equal_secret(const std::string_view left, const std::string_view right) {
    if (left.size() != right.size())
        return false;
    volatile unsigned int difference = 0U;
    for (std::size_t index = 0U; index < left.size(); ++index) {
        difference |=
            static_cast<unsigned char>(left[index]) ^ static_cast<unsigned char>(right[index]);
    }
    return difference == 0U;
}

/** @brief Produces one fixed-length random session token. */
bool token(std::array<char, 33>& destination) {
    std::array<unsigned char, 16> random{};
    if (!platform::web_random(random.data(), random.size()))
        return false;
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t index = 0U; index < random.size(); ++index) {
        destination[index * 2U] = hex[random[index] >> 4U];
        destination[index * 2U + 1U] = hex[random[index] & 15U];
    }
    destination[32] = '\0';
    return true;
}

/** @brief Replaces a response with a short fixed JSON error. */
void error(platform::WebResponse& response, const unsigned int status, const char* message) {
    response.status = status;
    response.content_type = "application/json";
    response.content_encoding = nullptr;
    response.asset = nullptr;
    const auto size =
        std::snprintf(response.body.data(), response.body.size(), "{\"error\":\"%s\"}", message);
    response.size = size < 0 || static_cast<std::size_t>(size) >= response.body.size()
                        ? 0U
                        : static_cast<std::size_t>(size);
}

/** @brief Appends a fixed API payload to the response without allocation. */
bool write(platform::WebResponse& response, const std::string_view value) {
    if (value.size() > response.body.size() - response.size)
        return false;
    std::memcpy(response.body.data() + response.size, value.data(), value.size());
    response.size += value.size();
    return true;
}

/** @brief Accepts only a local device address or assigned hostname. */
bool local_host(std::string_view host) {
    const auto colon = host.find(':');
    if (colon != std::string_view::npos) {
        if (host.substr(colon) != ":80")
            return false;
        host = host.substr(0U, colon);
    }
    if (host.empty())
        return false;
    const auto station = platform::network_info();
    if (station.address[0] != '\0' && !support::ascii_iequals(station.address, "0.0.0.0") &&
        support::ascii_iequals(host, station.address)) {
        return true;
    }
    const auto access_point = platform::access_point_info();
    if (access_point.address[0] != '\0' &&
        !support::ascii_iequals(access_point.address, "0.0.0.0") &&
        support::ascii_iequals(host, access_point.address)) {
        return true;
    }
    if (station.hostname[0] == '\0')
        return false;
    if (support::ascii_iequals(host, station.hostname))
        return true;
    constexpr std::string_view suffix = ".local";
    return host.size() > suffix.size() &&
           support::ascii_iequals(host.substr(host.size() - suffix.size()), suffix) &&
           support::ascii_iequals(host.substr(0U, host.size() - suffix.size()), station.hostname);
}

/** @brief Rejects cross-site browser requests and mismatched origins. */
bool same_origin(const platform::WebRequest& request) {
    const std::string_view site{request.fetch_site.data()};
    if (!site.empty() && site != "same-origin" && site != "none")
        return false;
    const std::string_view origin{request.origin.data()};
    if (origin.empty())
        return true;
    constexpr std::string_view scheme = "http://";
    return origin.substr(0U, scheme.size()) == scheme &&
           support::ascii_iequals(origin.substr(scheme.size()), request.host.data());
}

/** @brief Matches a bounded media type while allowing parameters. */
bool content_type_is(const std::string_view value, const std::string_view expected) {
    const auto semicolon = value.find(';');
    auto actual = value.substr(0U, semicolon);
    while (!actual.empty() && (actual.front() == ' ' || actual.front() == '\t'))
        actual.remove_prefix(1U);
    while (!actual.empty() && (actual.back() == ' ' || actual.back() == '\t'))
        actual.remove_suffix(1U);
    return actual == expected;
}

/** @brief Requires gzip to have a positive supported quality value. */
bool accepts_gzip(const std::string_view encodings) {
    auto remaining = encodings;
    while (!remaining.empty()) {
        const auto comma = remaining.find(',');
        auto item = support::trim_ascii(remaining.substr(0U, comma));
        const auto semicolon = item.find(';');
        if (support::ascii_iequals(support::trim_ascii(item.substr(0U, semicolon)), "gzip")) {
            if (semicolon == std::string_view::npos)
                return true;
            const auto parameter = support::trim_ascii(item.substr(semicolon + 1U));
            return parameter == "q=1" || parameter == "q=1.0" || parameter == "q=1.00" ||
                   parameter == "q=1.000";
        }
        if (comma == std::string_view::npos)
            break;
        remaining.remove_prefix(comma + 1U);
    }
    return false;
}

} // namespace

/** @brief Binds stored credentials and the shared bounded update service. */
RecoveryWebService::RecoveryWebService(Settings& settings, UpdateWebService& updates)
    : settings_(settings), updates_(updates) {}

/** @brief Revokes the browser session whenever the stored password changes. */
void RecoveryWebService::refresh_credential() {
    const auto password = settings_.web_password();
    if (equal_secret(password, {credential_.data(), credential_size_}))
        return;
    session_ = {};
    credential_.fill(0);
    credential_size_ = password.size();
    if (credential_size_ != 0U)
        std::memcpy(credential_.data(), password.data(), credential_size_);
}

/** @brief Returns the active unexpired session matching a request cookie. */
RecoveryWebService::Session* RecoveryWebService::session(
    std::string_view cookie, const std::uint32_t now) {
    std::string_view value;
    while (!cookie.empty()) {
        const auto end = cookie.find(';');
        const auto part = support::trim_ascii(cookie.substr(0U, end));
        constexpr std::string_view prefix = "nm_session=";
        if (part.substr(0U, prefix.size()) == prefix) {
            value = part.substr(prefix.size());
            break;
        }
        if (end == std::string_view::npos)
            break;
        cookie.remove_prefix(end + 1U);
    }
    if (session_.token[0] == '\0')
        return nullptr;
    if (now - session_.last_used >= 1800000U) {
        session_ = {};
        return nullptr;
    }
    return value.size() == 32U && equal_secret(value, session_.token.data()) ? &session_ : nullptr;
}

/** @brief Starts the recovery listener and handles one ready exchange. */
void RecoveryWebService::poll(const std::uint32_t now, const bool network_ready) {
    refresh_credential();
    if (!started_ && network_ready && (!attempted_start_ || now - last_start_ >= 5000U)) {
        started_ = platform::web_start();
        attempted_start_ = true;
        last_start_ = now;
        if (started_) {
            const auto network = platform::network_info();
            AddLog(LogLevel::info, "WEB: Safeboot recovery HTTP at %s",
                network.address[0] != '\0' ? network.address : "unknown address");
        } else {
            AddLog(LogLevel::error, "WEB: Safeboot HTTP start failed");
        }
    }
    if (auto* exchange = platform::web_request()) {
        handle(*exchange, now);
        platform::web_complete();
    }
}

/** @brief Validates and dispatches one recovery-only browser request. */
void RecoveryWebService::handle(platform::WebExchange& exchange, const std::uint32_t now) {
    refresh_credential();
    const auto& request = exchange.request;
    auto& response = exchange.response;
    const std::string_view path{request.path.data()};
    const bool post = request.method == platform::WebMethod::post;
    if (!local_host(request.host.data())) {
        error(response, 403U, "Host rejected");
        return;
    }
    if (!post && path == "/") {
        if (!accepts_gzip(request.accept_encoding.data())) {
            error(response, 406U, "Gzip required");
            return;
        }
        response.status = 200U;
        response.content_type = "text/html; charset=utf-8";
        response.content_encoding = "gzip";
        response.asset = reinterpret_cast<const char*>(generated::recovery_web::kIndex);
        response.size = sizeof(generated::recovery_web::kIndex);
        return;
    }
    if (!same_origin(request)) {
        error(response, 403U, "Origin rejected");
        return;
    }
    if (post) {
        const auto expected =
            path == "/api/update/chunk" ? "application/octet-stream" : "text/plain";
        if (!content_type_is(request.content_type.data(), expected)) {
            error(response, 415U, "Unsupported content type");
            return;
        }
    }
    if (!post && path == "/api/bootstrap") {
        response.status = 200U;
        response.content_type = "application/json";
        const auto size = std::snprintf(response.body.data(), response.body.size(),
            "{\"auth_required\":%s}", credential_size_ != 0U ? "true" : "false");
        response.size = size < 0 || static_cast<std::size_t>(size) >= response.body.size()
                            ? 0U
                            : static_cast<std::size_t>(size);
        return;
    }

    if (credential_size_ == 0U) {
        error(response, 403U, "Browser recovery requires a configured WebPassword");
        return;
    }

    auto* current = session(request.cookie.data(), now);
    if (post && path == "/api/login") {
        if (std::string_view{request.csrf.data()} != "login") {
            error(response, 403U, "Login header required");
            return;
        }
        if (now - login_window_ >= 30000U) {
            login_window_ = now;
            login_attempts_ = 0U;
        }
        if (login_attempts_ >= 5U) {
            error(response, 429U, "Try again later");
            return;
        }
        ++login_attempts_;
        if (!equal_secret(
                {request.body.data(), request.body_size}, {credential_.data(), credential_size_})) {
            error(response, 401U, "Login failed");
            return;
        }
        session_ = {};
        if (!token(session_.token) || !token(session_.csrf)) {
            session_ = {};
            error(response, 503U, "Session entropy unavailable");
            return;
        }
        current = &session_;
        current->last_used = now;
        std::snprintf(response.cookie.data(), response.cookie.size(),
            "nm_session=%s; Path=/; HttpOnly; SameSite=Strict%s", current->token.data(),
            request.secure ? "; Secure" : "");
        const auto size = std::snprintf(
            response.body.data(), response.body.size(), "{\"csrf\":\"%s\"}", current->csrf.data());
        response.content_type = "application/json";
        response.size = size < 0 || static_cast<std::size_t>(size) >= response.body.size()
                            ? 0U
                            : static_cast<std::size_t>(size);
        return;
    }

    if (current == nullptr) {
        error(response, 401U, "Login required");
        return;
    }
    if (!UpdateWebService::self_paced(post, path)) {
        if (now - current->rate_start >= 1000U) {
            current->rate_start = now;
            current->requests = 0U;
        }
        if (current->requests >= 25U) {
            error(response, 429U, "Request rate exceeded");
            return;
        }
        ++current->requests;
    }
    if (post && !equal_secret(request.csrf.data(), current->csrf.data())) {
        error(response, 403U, "CSRF token required");
        return;
    }
    current->last_used = now;
    if (!post && path == "/api/session") {
        const auto size = std::snprintf(response.body.data(), response.body.size(),
            "{\"auth_required\":true,\"csrf\":\"%s\"}", current->csrf.data());
        response.status = 200U;
        response.content_type = "application/json";
        response.size = size < 0 || static_cast<std::size_t>(size) >= response.body.size()
                            ? 0U
                            : static_cast<std::size_t>(size);
        return;
    }
    if (post && path == "/api/logout") {
        if (current != nullptr)
            *current = {};
        std::snprintf(response.cookie.data(), response.cookie.size(),
            "nm_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0%s",
            request.secure ? "; Secure" : "");
        response.content_type = "application/json";
        (void)write(response, "{}");
        return;
    }
    if (updates_.handle(request, response, now))
        return;
    error(response, 404U, "Endpoint not found");
}

} // namespace newmota
