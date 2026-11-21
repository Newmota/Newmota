/*
  log.h - logging interface for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace newmota {

enum class LogLevel : std::uint8_t {
    none,
    error,
    info,
    warning = info, // Preserve the existing persisted threshold values.
    debug,
    debug_more,
};

namespace log_detail {

// Keep independent thresholds so the platform and browser can receive
// different log levels without changing the persisted settings format.
inline std::uint8_t serial_log_level = static_cast<std::uint8_t>(LogLevel::info);
inline std::uint8_t web_log_level = static_cast<std::uint8_t>(LogLevel::info);

} // namespace log_detail

/** @brief Applies the runtime serial and web log thresholds. */
inline void ConfigureLogLevels(const std::uint8_t serial, const std::uint8_t web) {
    constexpr auto maximum = static_cast<std::uint8_t>(LogLevel::debug_more);
    if (serial <= maximum) {
        log_detail::serial_log_level = serial;
    }
    if (web <= maximum) {
        log_detail::web_log_level = web;
    }
}

/** @brief Room for the "HH:MM:SS.mmm " log timestamp and its terminator. */
inline constexpr std::size_t kLogTimestampCapacity = 14U;

/**
 * @brief Writes Tasmota's "HH:MM:SS.mmm " prefix from local time, or the time since boot.
 * Application images only; it does not fit the smallest Safeboot partition.
 * @return The prefix length.
 */
std::size_t WriteLogTimestamp(char* output, std::size_t capacity);

/**
 * @brief Supplies the local time for log timestamps.
 * @return false while no valid time is known, so lines show the time since boot instead.
 */
using LogClock = bool (*)(std::uint32_t& local_seconds, std::uint16_t& millisecond);

/** @brief Binds the clock behind log timestamps; null falls back to the time since boot. */
void SetLogClock(LogClock clock);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
/**
 * @brief Writes a formatted message when the requested log level is enabled. Application
 * images prefix it like Tasmota with "HH:MM:SS.mmm ". Call it on the application task only.
 * @param level Severity assigned to the message.
 * @param format printf-compatible format string followed by its arguments.
 */
void AddLog(LogLevel level, const char* format, ...);

} // namespace newmota
