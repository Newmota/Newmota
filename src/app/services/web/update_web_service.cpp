/*
  update_web_service.cpp - bounded browser firmware upload endpoint

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file update_web_service.cpp
 * @brief Bounded authenticated native firmware upload.
 */

#include "newmota_features.h"
#include "services/web/update_web_service.h"

#include "newmota/log.h"
#include "newmota/platform/platform.h"
#include "newmota/platform/update.h"
#include "newmota/platform/web.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace newmota {
namespace {

constexpr std::uint32_t kSessionTimeoutMilliseconds = 300000U;
constexpr std::size_t kMaximumUpdateChunkSize = 1024U;

/** @brief Keeps update chunks within the shared 1 KiB upload contract. */
std::size_t maximum_update_chunk_size(const platform::UpdateWriter& writer) {
    return std::min(
        {writer.maximum_chunk_size(), platform::WebRequest{}.body.size(), kMaximumUpdateChunkSize});
}

/** @brief Parses one unsigned decimal field without allocation or sign syntax. */
bool decimal(const std::string_view text, std::uint32_t& value) {
    if (text.empty())
        return false;
    value = 0U;
    for (const char character : text) {
        if (character < '0' || character > '9')
            return false;
        const auto digit = static_cast<std::uint32_t>(character - '0');
        if (value > (0xffffffffU - digit) / 10U)
            return false;
        value = value * 10U + digit;
    }
    return true;
}

/** @brief Parses the browser's standard Content-Range for one sequential chunk. */
bool parse_content_range(
    const std::string_view text, std::uint32_t& offset, std::uint32_t& end, std::uint32_t& total) {
    constexpr std::string_view prefix = "bytes ";
    if (text.substr(0, prefix.size()) != prefix)
        return false;
    const auto range = text.substr(prefix.size());
    const auto dash = range.find('-');
    const auto slash = range.find('/');
    if (dash == std::string_view::npos || slash == std::string_view::npos || dash == 0U ||
        slash <= dash + 1U || slash + 1U >= range.size() ||
        range.find('-', dash + 1U) != std::string_view::npos ||
        range.find('/', slash + 1U) != std::string_view::npos) {
        return false;
    }
    return decimal(range.substr(0U, dash), offset) &&
           decimal(range.substr(dash + 1U, slash - dash - 1U), end) &&
           decimal(range.substr(slash + 1U), total) && end >= offset;
}

/**
 * @brief Writes a bounded JSON result using only fixed response storage; logs every error.
 *
 * The progress steps below are application-only: with them the esp32c2-2m Safeboot overflows
 * its fixed partition. Errors are logged in both images.
 */
void result(platform::WebResponse& response, const unsigned int status, const char* state,
    const char* error = nullptr) {
    if (error != nullptr)
        AddLog(LogLevel::debug, "UPL: %u %s", status, error);
    response.status = status;
    response.content_type = "application/json";
    response.content_encoding = nullptr;
    response.asset = nullptr;
    const auto size = error == nullptr ? std::snprintf(response.body.data(), response.body.size(),
                                             "{\"state\":\"%s\"}", state)
                                       : std::snprintf(response.body.data(), response.body.size(),
                                             "{\"state\":\"%s\",\"error\":\"%s\"}", state, error);
    response.size = size < 0 || static_cast<std::size_t>(size) >= response.body.size()
                        ? 0U
                        : static_cast<std::size_t>(size);
}

/** @brief Converts a session result into a stable HTTP status and short message. */
void session_error(platform::WebResponse& response, const UpdateSessionResult update_result) {
    switch (update_result) {
    case UpdateSessionResult::success:
        result(response, 200U, "ok");
        return;
    case UpdateSessionResult::unavailable:
        result(response, 503U, "failed", "Update unavailable");
        return;
    case UpdateSessionResult::busy:
        result(response, 409U, "failed", "Update busy");
        return;
    case UpdateSessionResult::invalid_size:
        result(response, 413U, "failed", "Image size rejected");
        return;
    case UpdateSessionResult::image_rejected:
        result(response, 422U, "failed", "Image rejected");
        return;
    case UpdateSessionResult::io_error:
        result(response, 500U, "failed", "Flash operation failed");
        return;
    case UpdateSessionResult::invalid_state:
    case UpdateSessionResult::invalid_offset:
    case UpdateSessionResult::invalid_chunk:
    case UpdateSessionResult::incomplete:
        result(response, 409U, "failed", "Update request out of sequence");
        return;
    }
    result(response, 500U, "failed", "Update failed");
}

/** @brief Maps internal update states to their stable JSON names. */
const char* session_state(const UpdateSessionState state) {
    switch (state) {
    case UpdateSessionState::idle:
        return "idle";
    case UpdateSessionState::receiving:
        return "receiving";
    case UpdateSessionState::verified:
        return "verified";
    case UpdateSessionState::activated:
        return "activated";
    case UpdateSessionState::failed:
        return "failed";
    }
    return "failed";
}

/** @brief Matches a media type while allowing bounded optional parameters. */
bool content_type_is(const std::string_view value, const std::string_view expected) {
    const auto separator = value.find(';');
    auto actual = value.substr(0U, separator);
    while (!actual.empty() && (actual.front() == ' ' || actual.front() == '\t'))
        actual.remove_prefix(1U);
    while (!actual.empty() && (actual.back() == ' ' || actual.back() == '\t'))
        actual.remove_suffix(1U);
    return actual == expected;
}

} // namespace

/** @brief Binds the native writer and delayed restart controller. */
UpdateWebService::UpdateWebService(platform::UpdateWriter& writer, RestartController& restart)
    : writer_(writer), session_(writer), restart_(restart) {}

/** @brief Handles one authenticated update request without network I/O. */
bool UpdateWebService::handle(
    const platform::WebRequest& request, platform::WebResponse& response, const std::uint32_t now) {
    const std::string_view path{request.path.data()};
    const bool post = request.method == platform::WebMethod::post;
    if (path.substr(0U, 12U) == "/api/update/" && !self_paced(post, path)) {
        if (now - update_rate_window_ >= 1000U) {
            update_rate_window_ = now;
            update_requests_ = 0U;
        }
        if (update_requests_ >= 25U) {
            result(response, 429U, "failed", "Update request rate exceeded");
            return true;
        }
        ++update_requests_;
    }
    if (path == "/api/update/capabilities" && !post) {
        const auto state = session_.state();
        const bool session_active =
            state == UpdateSessionState::receiving || state == UpdateSessionState::verified;
        const auto chunk_size = maximum_update_chunk_size(writer_);
        const auto minimum_chunk = writer_.minimum_initial_chunk_size();
        const bool transport_usable =
            chunk_size != 0U && minimum_chunk != 0U && minimum_chunk <= chunk_size;
        response.content_type = "application/json";
        response.content_encoding = nullptr;
        response.asset = nullptr;
        const auto size = std::snprintf(response.body.data(), response.body.size(),
            "{\"available\":%s,\"max_image_size\":%lu,"
            "\"chunk_size\":%lu,\"minimum_initial_chunk\":%lu,"
            "\"received_bytes\":%lu,\"expected_bytes\":%lu,"
            "\"state\":\"%s\"}",
            (transport_usable && (session_active || writer_.available())) ? "true" : "false",
            static_cast<unsigned long>(writer_.maximum_image_size()),
            static_cast<unsigned long>(chunk_size), static_cast<unsigned long>(minimum_chunk),
            static_cast<unsigned long>(session_.received_bytes()),
            static_cast<unsigned long>(session_.expected_bytes()), session_state(state));
        response.size = size < 0 || static_cast<std::size_t>(size) >= response.body.size()
                            ? 0U
                            : static_cast<std::size_t>(size);
        response.status = 200U;
        return true;
    }
    if (path.substr(0U, 12U) != "/api/update/")
        return false;

    const auto operation = path.substr(12U);
    if (!post) {
        result(response, 404U, "failed", "Update endpoint not found");
        return true;
    }
    if (operation == "chunk") {
        if (!content_type_is(request.content_type.data(), "application/octet-stream")) {
            result(response, 415U, "failed", "Use application/octet-stream");
            return true;
        }
        std::uint32_t offset = 0U, end = 0U, total = 0U;
        if (!parse_content_range(request.content_range.data(), offset, end, total) ||
            request.body_size == 0U || request.body_size > kMaximumUpdateChunkSize ||
            static_cast<std::uint64_t>(end) - offset + 1U != request.body_size ||
            total != session_.expected_bytes() || end >= total) {
            (void)session_.abort();
            result(response, 400U, "failed", "Invalid Content-Range");
            return true;
        }
        const auto written = session_.write(offset, {request.body.data(), request.body_size});
        if (written != UpdateSessionResult::success) {
            (void)session_.abort();
            session_error(response, written);
            return true;
        }
        last_activity_ = now;
        const auto size = std::snprintf(response.body.data(), response.body.size(),
            "{\"state\":\"receiving\",\"received_bytes\":%lu,"
            "\"expected_bytes\":%lu}",
            static_cast<unsigned long>(session_.received_bytes()),
            static_cast<unsigned long>(session_.expected_bytes()));
        response.content_type = "application/json";
        response.content_encoding = nullptr;
        response.asset = nullptr;
        response.status = 200U;
        response.size = size < 0 || static_cast<std::size_t>(size) >= response.body.size()
                            ? 0U
                            : static_cast<std::size_t>(size);
        return true;
    }
    if (!content_type_is(request.content_type.data(), "text/plain")) {
        result(response, 415U, "failed", "Use text/plain");
        return true;
    }

    if (operation == "begin") {
        const auto chunk_size = maximum_update_chunk_size(writer_);
        const auto minimum_chunk = writer_.minimum_initial_chunk_size();
        if (chunk_size == 0U || minimum_chunk == 0U || minimum_chunk > chunk_size) {
            result(response, 503U, "failed", "Update chunk transport unavailable");
            return true;
        }
        std::uint32_t image_size = 0U;
        if (!decimal({request.body.data(), request.body_size}, image_size)) {
            result(response, 400U, "failed", "Invalid update request");
            return true;
        }
        const auto started = session_.begin(image_size);
        if (started != UpdateSessionResult::success) {
            if (session_.state() == UpdateSessionState::failed)
                (void)session_.abort();
            session_error(response, started);
            return true;
        }
        last_activity_ = now;
#ifndef NEWMOTA_IMAGE_SAFEBOOT
        AddLog(LogLevel::debug, "UPL: receiving %lu bytes", static_cast<unsigned long>(image_size));
#endif
        result(response, 202U, "receiving");
        return true;
    }
    if (request.body_size != 0U) {
        result(response, 400U, "failed", "Unexpected request body");
        return true;
    }
    if (operation == "finish") {
#ifndef NEWMOTA_IMAGE_SAFEBOOT
        // The writer reads the staged image back; the time shows how close it runs to the
        // web transport's wait for an answer.
        const auto started = platform::monotonic_milliseconds();
#endif
        const auto finished = session_.finish();
#ifndef NEWMOTA_IMAGE_SAFEBOOT
        const auto elapsed = platform::monotonic_milliseconds() - started;
#endif
        if (finished != UpdateSessionResult::success) {
            if (session_.state() == UpdateSessionState::failed)
                (void)session_.abort();
#ifndef NEWMOTA_IMAGE_SAFEBOOT
            AddLog(LogLevel::debug, "UPL: check failed after %lu ms",
                static_cast<unsigned long>(elapsed));
#endif
            session_error(response, finished);
            return true;
        }
#ifndef NEWMOTA_IMAGE_SAFEBOOT
        AddLog(LogLevel::debug, "UPL: %lu bytes verified in %lu ms",
            static_cast<unsigned long>(session_.received_bytes()),
            static_cast<unsigned long>(elapsed));
#endif
        last_activity_ = now;
        result(response, 200U, "verified");
        return true;
    }
    if (operation == "activate") {
        if (restart_.pending()) {
            result(response, 409U, "failed", "Restart already pending");
            return true;
        }
        const auto activated = session_.activate();
        if (activated != UpdateSessionResult::success) {
            (void)session_.abort();
            session_error(response, activated);
            return true;
        }
        const auto restart = restart_.schedule(RestartTarget::current, now);
        if (restart != RestartRequestResult::scheduled) {
            result(response, 500U, "activated", "Restart scheduling failed");
            return true;
        }
#ifndef NEWMOTA_IMAGE_SAFEBOOT
        AddLog(LogLevel::debug, "UPL: image activated; restarting");
#endif
        result(response, 202U, "restarting");
        return true;
    }
    if (operation == "abort") {
#ifndef NEWMOTA_IMAGE_SAFEBOOT
        AddLog(LogLevel::debug, "UPL: aborted by the browser at %lu of %lu bytes",
            static_cast<unsigned long>(session_.received_bytes()),
            static_cast<unsigned long>(session_.expected_bytes()));
#endif
        const auto aborted = session_.abort();
        if (aborted != UpdateSessionResult::success) {
            session_error(response, aborted);
            return true;
        }
        result(response, 200U, "idle");
        return true;
    }
    result(response, 404U, "failed", "Update endpoint not found");
    return true;
}

/** @brief Aborts an update session that stopped making progress. */
void UpdateWebService::poll(const std::uint32_t now) {
    if ((session_.state() == UpdateSessionState::receiving ||
            session_.state() == UpdateSessionState::verified) &&
        static_cast<std::int32_t>(now - last_activity_) >=
            static_cast<std::int32_t>(kSessionTimeoutMilliseconds)) {
#ifndef NEWMOTA_IMAGE_SAFEBOOT
        AddLog(LogLevel::debug, "UPL: no progress for %lu s; aborted",
            static_cast<unsigned long>(kSessionTimeoutMilliseconds / 1000U));
#endif
        (void)session_.abort();
        last_activity_ = 0U;
    }
}

} // namespace newmota
