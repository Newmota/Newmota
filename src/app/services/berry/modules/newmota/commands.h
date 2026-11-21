/*
  commands.h - Tasmota-style Berry commands

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/**
 * @file commands.h
 * @brief `newmota.add_cmd()` commands, reached when no native command matches, and the
 * `resp_cmnd*` functions that set their response.
 */

#pragma once

#include "core/command/command.h"

#include <array>
#include <cstddef>
#include <string_view>

struct bvm;

namespace newmota {

/** @brief One Berry command invocation; the caller owns the response buffer. */
struct BerryCommandCall final {
    const CommandRequest* request{};
    int command{-1};             ///< Registered command, from find_berry_command().
    std::array<char, 33> name{}; ///< The command name as registered.
    char* response{};            ///< kMaximumCommandResponseLength bytes.
    std::size_t response_size{};
    bool response_overflow{};
};

/** @brief Finds a registered command, ignoring case, as Tasmota's find_key_i().
 * @return Its index for BerryCommandCall::command, or -1.
 */
[[nodiscard]] int find_berry_command(std::string_view name);
/** @brief Makes @p call the target of `resp_cmnd*` and returns the previous target. */
BerryCommandCall* swap_berry_command_call(BerryCommandCall* call);
/** @brief Calls the command: f(name, idx, payload, payload_json); argument: call comptr. */
int dispatch_berry_command(bvm* vm);

/** @brief `newmota.add_cmd(name, function)`. */
int add_berry_command(bvm* vm);
/** @brief `newmota.remove_cmd(name)`. */
int remove_berry_command(bvm* vm);
/** @brief `newmota.resp_cmnd(json)`: the response is @p json as given. */
int berry_resp_cmnd(bvm* vm);
/** @brief `newmota.resp_cmnd_str(text)`: `{"<Command>":"<text>"}`. */
int berry_resp_cmnd_str(bvm* vm);
/** @brief `newmota.resp_cmnd_done()`: `{"<Command>":"Done"}`. */
int berry_resp_cmnd_done(bvm* vm);
/** @brief `newmota.resp_cmnd_error()`: `{"<Command>":"Error"}`. */
int berry_resp_cmnd_error(bvm* vm);
/** @brief `newmota.resp_cmnd_failed()`: `{"<Command>":"Failed"}`. */
int berry_resp_cmnd_failed(bvm* vm);
/** @brief Releases all commands before VM destruction. */
void remove_berry_commands(bvm* vm);

} // namespace newmota
