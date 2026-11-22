/*
  defer.h - one-shot deferred commands for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file defer.h
 * @brief `Defer <ms> <command>`: runs one command line later from the main loop.
 */

#pragma once

#include "core/command/command.h"

#include <cstddef>
#include <cstdint>

namespace newmota::defer {

/** @brief Most commands waiting at the same time. */
inline constexpr std::size_t kSlotCount = 8U;
/** @brief Longest deferred command line in bytes. */
inline constexpr std::size_t kLineCapacity = 128U;
/** @brief Longest delay: one hour. */
inline constexpr std::uint32_t kMaximumMilliseconds = 3600000U;

/**
 * @brief Handles `Defer`.
 * @return unknown for every other command name.
 */
[[nodiscard]] CommandStatus command(const CommandRequest& request, CommandOutput& output);

/** @brief Runs the earliest due command line, at most one per main-loop pass. */
void poll(CommandProcessor& processor, std::uint32_t now);

/** @brief Drops every waiting command line, as a restart does. */
void clear();

} // namespace newmota::defer
