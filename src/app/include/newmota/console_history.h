/*
  console_history.h - bounded browser console output histories

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace newmota::console_history {

/** @brief Maximum bytes retained in one console entry. */
inline constexpr std::size_t kWebLineCapacity = 192U;
/** @brief Lines of one Berry traceback shown in the Berry Console; the rest goes to the log. */
inline constexpr std::size_t kBerryTraceLines = 8U;

struct ReadResult final {
    std::uint32_t next{};
    std::size_t count{};
    bool dropped{};
    bool reset{};
    bool more{};
};

enum class OutputKind : std::uint8_t {
    log,
    command_result,
};

using EventWriter = bool (*)(void* context, std::uint32_t sequence, OutputKind kind,
    std::uint32_t group, std::string_view text);

/*
 * One 2 KiB ring holds Web Console and Berry Console lines while somebody can read them:
 * from boot for 5 minutes (the boot log), then only while a console polls, until 30 s after
 * its last poll. Otherwise nothing is allocated and lines are not retained.
 */

/** @brief Adds one log or command result line to the Web Console history. */
void record_web_output(
    std::string_view text, OutputKind kind = OutputKind::log, std::uint32_t group = 0U);

/** @brief Adds one Berry stdout line to the Berry Console history. */
void record_berry_output(std::string_view text);

/** @brief Starts or renews a viewer lease before a console poll reads; false if out of memory. */
[[nodiscard]] bool attach_viewer();

/** @brief Ends the boot window or an expired viewer lease and frees the ring. */
void maintain(std::uint32_t now);

/** @brief Writes up to max_events after cursor without consuming shared history. */
[[nodiscard]] ReadResult read_web_output(
    std::uint32_t cursor, std::size_t max_events, EventWriter writer, void* context);

/** @brief Writes up to max_events after cursor without consuming shared history. */
[[nodiscard]] ReadResult read_berry_output(
    std::uint32_t cursor, std::size_t max_events, EventWriter writer, void* context);

} // namespace newmota::console_history
