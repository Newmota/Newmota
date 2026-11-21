/*
  log.cpp - bounded logging implementation for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/log.h"

#include "newmota_features.h"
#if USE_WEB_CONSOLE
#include "newmota/console_history.h"
#endif

#include "newmota/platform/platform.h"
#if USE_TASMOTA_STATE
#include "core/system/uptime.h"
#endif

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <string_view>

namespace newmota {
namespace {

constexpr std::size_t kLogLineCapacity = 192;

#if USE_TIME
LogClock log_clock{};
#endif

/**
 * @brief Tests whether a message passes one configured sink threshold.
 */
[[nodiscard]] constexpr bool IsEnabled(const LogLevel level, const std::uint8_t threshold) {
    return level != LogLevel::none && static_cast<std::uint8_t>(level) <= threshold;
}

} // namespace

#if USE_TASMOTA_STATE
std::size_t WriteLogTimestamp(char* const output, const std::size_t capacity) {
    std::uint32_t seconds{};
    std::uint16_t millisecond{};
#if USE_TIME
    if (log_clock == nullptr || !log_clock(seconds, millisecond))
#endif
    {
        seconds = uptime_seconds();
        millisecond = static_cast<std::uint16_t>(platform::monotonic_milliseconds() % 1000U);
    }
    const int length = std::snprintf(output, capacity, "%02u:%02u:%02u.%03u ",
        static_cast<unsigned>(seconds / 3600U % 24U), static_cast<unsigned>(seconds / 60U % 60U),
        static_cast<unsigned>(seconds % 60U), static_cast<unsigned>(millisecond));
    return length > 0 ? static_cast<std::size_t>(length) : 0U;
}
#endif

#if USE_TIME
void SetLogClock(const LogClock clock) {
    log_clock = clock;
}
#endif

/**
 * @brief Formats one bounded log line and forwards it to the platform sink.
 */
void AddLog(const LogLevel level, const char* const format, ...) {
    const bool serial_enabled = IsEnabled(level, log_detail::serial_log_level);
#if USE_WEB_CONSOLE
    const bool web_enabled = IsEnabled(level, log_detail::web_log_level);
    if (!serial_enabled && !web_enabled) {
#else
    if (!serial_enabled) {
#endif
        return;
    }

    std::array<char, kLogLineCapacity> line{};
    va_list arguments;
    va_start(arguments, format);
    std::size_t prefix = 0U;
#if USE_TASMOTA_STATE
    prefix = WriteLogTimestamp(line.data(), line.size());
#endif
    const int length =
        std::vsnprintf(line.data() + prefix, line.size() - prefix, format, arguments);
    va_end(arguments);

    if (length <= 0) {
        return;
    }

    const bool truncated = static_cast<std::size_t>(length) >= line.size() - prefix;
    if (truncated) {
        line[line.size() - 4] = '.';
        line[line.size() - 3] = '.';
        line[line.size() - 2] = '.';
    }
    const auto size = std::min(prefix + static_cast<std::size_t>(length), line.size() - 1);
    const std::string_view message{line.data(), size};
    if (serial_enabled) {
        platform::write_log(message);
    }
#if USE_WEB_CONSOLE
    if (web_enabled) {
        console_history::record_web_output(message);
    }
#endif
}

} // namespace newmota
