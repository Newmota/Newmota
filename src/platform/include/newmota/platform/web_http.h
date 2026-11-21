/*
  web_http.h - Bounded HTTP stream framing helpers

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#pragma once
#include "newmota/platform/web.h"
#include <cstdint>
#include <cstring>
#include <string_view>

/** @file web_http.h
 * @brief Bounded HTTP stream framing helpers shared by native adapters.
 */
namespace newmota::platform::web_http {
constexpr std::size_t header_limit = 2048;

/** @brief Compares one bounded ASCII field without locale or allocation. */
[[nodiscard]] static inline bool ascii_iequals(
    const std::string_view left, const std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        auto a = static_cast<unsigned char>(left[index]);
        auto b = static_cast<unsigned char>(right[index]);
        if (a >= 'A' && a <= 'Z')
            a = static_cast<unsigned char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z')
            b = static_cast<unsigned char>(b + ('a' - 'A'));
        if (a != b)
            return false;
    }
    return true;
}

/** @brief Removes HTTP optional whitespace from both ends of a field. */
[[nodiscard]] static inline std::string_view trim_ows(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
        value.remove_suffix(1);
    return value;
}

/** @brief Copies one header field into its fixed request slot. */
template <std::size_t N>
[[nodiscard]] static bool copy_field(
    const std::string_view value, std::array<char, N>& destination) {
    if (value.size() >= N)
        return false;
    if (!value.empty())
        std::memcpy(destination.data(), value.data(), value.size());
    destination[value.size()] = '\0';
    return true;
}

/** @brief Parses an unsigned decimal Content-Length without libc allocation. */
[[nodiscard]] static inline bool decimal(const std::string_view value, std::size_t& result) {
    if (value.empty())
        return false;
    std::size_t parsed = 0;
    for (const auto character : value) {
        if (character < '0' || character > '9')
            return false;
        const auto digit = static_cast<std::size_t>(character - '0');
        if (parsed > (static_cast<std::size_t>(-1) - digit) / 10U)
            return false;
        parsed = parsed * 10U + digit;
    }
    result = parsed;
    return true;
}

/** @brief Finds the first complete HTTP header block in the receive buffer. */
[[nodiscard]] static inline std::size_t header_end(
    const char* receive_buffer, const std::size_t receive_size) {
    if (receive_size < 4)
        return 0;
    for (std::size_t index = 0; index + 4 <= receive_size; ++index) {
        if (receive_buffer[index] == '\r' && receive_buffer[index + 1] == '\n' &&
            receive_buffer[index + 2] == '\r' && receive_buffer[index + 3] == '\n') {
            return index + 4;
        }
    }
    return 0;
}

enum class ParseResult : std::uint8_t { incomplete, ready, rejected };

/** @brief Parses one complete request, preserving bounded fields for the app. */
template <bool Persistent = true>
[[nodiscard]] static inline ParseResult parse_request(char* receive_buffer,
    std::size_t& receive_size, WebRequest& request, bool& request_close,
    const std::size_t headers_end, unsigned int& rejection_status) {
    if (headers_end > header_limit) {
        rejection_status = 431;
        return ParseResult::rejected;
    }

    const std::string_view headers{receive_buffer, headers_end};
    const auto first_line_end = headers.find("\r\n");
    if (first_line_end == std::string_view::npos) {
        rejection_status = 400;
        return ParseResult::rejected;
    }
    const auto first_line = headers.substr(0, first_line_end);
    request.reset();
    std::size_t method_size = 0;
    if (first_line.substr(0, 4) == "GET ") {
        request.method = WebMethod::get;
        method_size = 4;
    } else if (first_line.substr(0, 5) == "POST ") {
        request.method = WebMethod::post;
        method_size = 5;
    } else {
        rejection_status = 400;
        return ParseResult::rejected;
    }
    const auto path_end = first_line.find(' ', method_size);
    if (path_end == std::string_view::npos || path_end == method_size ||
        path_end - method_size >= request.path.size()) {
        rejection_status = path_end == std::string_view::npos ? 400 : 414;
        return ParseResult::rejected;
    }
    const auto version = first_line.substr(path_end + 1);
    if (version != "HTTP/1.0" && version != "HTTP/1.1") {
        rejection_status = 400;
        return ParseResult::rejected;
    }
    if (!copy_field(first_line.substr(method_size, path_end - method_size), request.path)) {
        rejection_status = 414;
        return ParseResult::rejected;
    }

    request_close = !Persistent || version != "HTTP/1.1";
    std::size_t content_length = 0;
    bool has_content_length = false;
    std::size_t line_start = first_line_end + 2;
    while (line_start + 2 <= headers_end) {
        const auto line_end = headers.find("\r\n", line_start);
        if (line_end == std::string_view::npos || line_end < line_start) {
            rejection_status = 400;
            return ParseResult::rejected;
        }
        if (line_end == line_start)
            break;
        const auto line = headers.substr(line_start, line_end - line_start);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            rejection_status = 400;
            return ParseResult::rejected;
        }
        const auto name = line.substr(0, colon);
        if constexpr (Persistent)
            for (const auto character : name) {
                const bool token =
                    (character >= '0' && character <= '9') ||
                    (character >= 'A' && character <= 'Z') ||
                    (character >= 'a' && character <= 'z') ||
                    std::string_view{"!#$%&'*+-.^_`|~"}.find(character) != std::string_view::npos;
                if (!token) {
                    rejection_status = 400;
                    return ParseResult::rejected;
                }
            }
        const auto value = trim_ows(line.substr(colon + 1));
        bool copied = true;
        if (ascii_iequals(name, "Host")) {
            copied = copy_field(value, request.host);
        } else if (ascii_iequals(name, "Origin")) {
            copied = copy_field(value, request.origin);
        } else if (ascii_iequals(name, "Cookie")) {
            copied = copy_field(value, request.cookie);
        } else if (ascii_iequals(name, "X-Newmota-CSRF")) {
            copied = copy_field(value, request.csrf);
        } else if (ascii_iequals(name, "Content-Type")) {
            copied = copy_field(value, request.content_type);
        } else if (ascii_iequals(name, "Content-Range")) {
            copied = copy_field(value, request.content_range);
        } else if (ascii_iequals(name, "Sec-Fetch-Site")) {
            copied = copy_field(value, request.fetch_site);
        } else if (ascii_iequals(name, "Accept-Encoding")) {
            copied = copy_field(value, request.accept_encoding);
        } else if (Persistent && ascii_iequals(name, "Connection")) {
            auto tokens = value;
            while (!tokens.empty()) {
                const auto comma = tokens.find(',');
                if (ascii_iequals(trim_ows(tokens.substr(0, comma)), "close"))
                    request_close = true;
                if (comma == std::string_view::npos)
                    break;
                tokens.remove_prefix(comma + 1);
            }
        } else if (ascii_iequals(name, "Content-Length")) {
            if (has_content_length || !decimal(value, content_length)) {
                rejection_status = 400;
                return ParseResult::rejected;
            }
            has_content_length = true;
        } else if (ascii_iequals(name, "Transfer-Encoding")) {
            // Chunked requests would need a separate bounded decoder.  Refuse
            // them explicitly instead of guessing at body framing.
            rejection_status = 400;
            return ParseResult::rejected;
        }
        if (!copied) {
            rejection_status = 431;
            return ParseResult::rejected;
        }
        line_start = line_end + 2;
    }

    if (content_length > request.body.size()) {
        rejection_status = 413;
        return ParseResult::rejected;
    }
    if (request.method == WebMethod::get && content_length != 0) {
        rejection_status = 400;
        return ParseResult::rejected;
    }
    if (receive_size < headers_end + content_length)
        return ParseResult::incomplete;
    request.body_size = content_length;
    if (content_length != 0) {
        std::memcpy(request.body.data(), receive_buffer + headers_end, content_length);
    }
    if constexpr (Persistent) {
        const auto consumed = headers_end + content_length;
        receive_size -= consumed;
        std::memmove(receive_buffer, receive_buffer + consumed, receive_size);
    } else {
        receive_size = 0;
    }
    return ParseResult::ready;
}

} // namespace newmota::platform::web_http
