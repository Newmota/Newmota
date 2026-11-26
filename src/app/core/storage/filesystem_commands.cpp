/*
  filesystem_commands.cpp - filesystem commands for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "core/storage/filesystem_commands.h"

#include "newmota_features.h"

#include "core/storage/filesystem.h"
#include "support/text.h"
#include "support/number.h"

#include <cstddef>
#include <string_view>

namespace newmota::commands {
namespace {

using Handler = CommandStatus (*)(const CommandRequest&, CommandOutput&);

struct CommandDefinition final {
    const char* name;
    Handler handler;
};

enum class ArgumentResult : std::uint8_t {
    missing,
    parsed,
    invalid,
};

/**
 * @brief Parses an exact number of optionally quoted command arguments.
 * @return Whether arguments were missing, parsed, or invalid.
 */
[[nodiscard]] ArgumentResult parse_arguments(const std::string_view payload,
    std::string_view* const arguments, const std::size_t expected_count) {
    std::size_t position = 0;
    for (std::size_t argument = 0; argument < expected_count; ++argument) {
        while (position < payload.size() && support::ascii_is_space(payload[position])) {
            ++position;
        }
        if (position == payload.size()) {
            return ArgumentResult::missing;
        }

        const char quote =
            payload[position] == '\'' || payload[position] == '"' ? payload[position++] : '\0';
        const std::size_t start = position;
        if (quote != '\0') {
            while (position < payload.size() && payload[position] != quote) {
                if (payload[position] == '\0') {
                    return ArgumentResult::invalid;
                }
                ++position;
            }
            if (position == payload.size()) {
                return ArgumentResult::invalid;
            }
            arguments[argument] = payload.substr(start, position - start);
            ++position;
            if (position < payload.size() && !support::ascii_is_space(payload[position])) {
                return ArgumentResult::invalid;
            }
        } else {
            while (position < payload.size() && !support::ascii_is_space(payload[position])) {
                if (payload[position] == '\0') {
                    return ArgumentResult::invalid;
                }
                ++position;
            }
            arguments[argument] = payload.substr(start, position - start);
        }
        if (arguments[argument].empty()) {
            return ArgumentResult::invalid;
        }
    }
    while (position < payload.size() && support::ascii_is_space(payload[position])) {
        ++position;
    }
    if (position != payload.size()) {
        return ArgumentResult::invalid;
    }
    return ArgumentResult::parsed;
}

/**
 * @brief Checks that a command has no index and carries only query syntax.
 */
[[nodiscard]] bool valid_query_only(const CommandRequest& request) {
    return !request.has_index && request.is_query();
}

/**
 * @brief Writes one volume's `"Mounted"` and byte usage members.
 * @return false when the space query failed; the members then carry `"Error"`.
 */
bool write_space(CommandOutput& output, const filesystem::Volume volume) {
    if (!filesystem::is_mounted(volume))
        return output.write("\"Mounted\":false");
    filesystem::SpaceInfo space{};
    if (!filesystem::space(space, volume)) {
        (void)output.format("\"Mounted\":true,\"Error\":%d", filesystem::last_error());
        return false;
    }
    return output.format("\"Mounted\":true,\"Total\":%s,\"Used\":%s,\"Free\":%s",
        newmota::support::decimal(std::uint64_t(space.total_bytes)).text,
        newmota::support::decimal(std::uint64_t(space.used_bytes)).text,
        newmota::support::decimal(
            std::uint64_t(
                space.used_bytes < space.total_bytes ? space.total_bytes - space.used_bytes : 0U))
            .text);
}

/**
 * @brief Handles the df command and reports filesystem byte usage.
 *
 * With `USE_SDCARD` an `"SD"` object reports the card the same way.
 */
CommandStatus df_command(const CommandRequest& request, CommandOutput& output) {
    if (!valid_query_only(request)) {
        return CommandStatus::invalid;
    }
    bool ok = output.write("{\"df\":{") && write_space(output, filesystem::Volume::flash);
#if USE_SDCARD
    const bool card = output.write(",\"SD\":{") && write_space(output, filesystem::Volume::sd);
    ok = output.write("}") && card && ok;
#endif
    return output.write("}}") && ok ? CommandStatus::handled : CommandStatus::failed;
}

struct ListOutput final {
    CommandOutput* output;
    bool first{true};
};

/**
 * @brief Serializes one visited directory entry into the active ls response.
 * @return false when response output fails.
 */
bool write_directory_entry(void* const context, const filesystem::DirectoryEntry& entry) {
    auto& state = *static_cast<ListOutput*>(context);
    auto& output = *state.output;
    if (!state.first && !output.write(",")) {
        return false;
    }
    state.first = false;
    if (!output.write("{\"Name\":")) {
        return false;
    }
    if (!output.json_string(entry.name)) {
        return false;
    }
    if (!output.write(",\"Type\":") ||
        !output.json_string(
            entry.type == filesystem::EntryType::directory ? "directory" : "file")) {
        return false;
    }
    return output.format(",\"Size\":%lu}", static_cast<unsigned long>(entry.size_bytes));
}

/**
 * @brief Handles ls queries for the root or one supplied path.
 */
CommandStatus ls_command(const CommandRequest& request, CommandOutput& output) {
    if (request.has_index) {
        return CommandStatus::invalid;
    }

    std::string_view path = "/";
    if (!request.is_query()) {
        const auto result = parse_arguments(request.payload, &path, 1);
        if (result != ArgumentResult::parsed) {
            return CommandStatus::invalid;
        }
    }

    if (!output.write("{\"ls\":{\"Path\":") || !output.json_string(path) ||
        !output.write(",\"Entries\":[")) {
        return CommandStatus::failed;
    }

    ListOutput list_output{&output};
    const auto result = filesystem::list(path, write_directory_entry, &list_output);
    if (result == filesystem::DirectoryResult::output_stopped || output.failed()) {
        return CommandStatus::failed;
    }
    if (result == filesystem::DirectoryResult::listed) {
        return output.write("]}}") ? CommandStatus::handled : CommandStatus::failed;
    }

    const char* message = "Filesystem unavailable";
    switch (result) {
    case filesystem::DirectoryResult::invalid_path:
        message = "Invalid path";
        break;
    case filesystem::DirectoryResult::open_failed:
        message = "Open failed";
        break;
    case filesystem::DirectoryResult::read_failed:
        message = "Read failed";
        break;
    case filesystem::DirectoryResult::unavailable:
    case filesystem::DirectoryResult::listed:
    case filesystem::DirectoryResult::output_stopped:
        break;
    }
    (void)output.format("],\"Error\":\"%s\",\"Code\":%d}}", message, filesystem::last_error());
    return CommandStatus::failed;
}

/**
 * @brief Handles removal of one file or empty directory.
 */
CommandStatus rm_command(const CommandRequest& request, CommandOutput& output) {
    if (request.has_index || request.is_query()) {
        return CommandStatus::invalid;
    }

    std::string_view path{};
    if (parse_arguments(request.payload, &path, 1) != ArgumentResult::parsed) {
        return CommandStatus::invalid;
    }

    const auto result = filesystem::remove(path);
    if (!output.write("{\"rm\":{\"Path\":") || !output.json_string(path)) {
        return CommandStatus::failed;
    }
    switch (result) {
    case filesystem::MutationResult::succeeded:
        return output.write(",\"Result\":\"Removed\"}}") ? CommandStatus::handled
                                                         : CommandStatus::failed;
    case filesystem::MutationResult::unavailable:
        (void)output.write(",\"Error\":\"Filesystem unavailable\"}}");
        return CommandStatus::failed;
    case filesystem::MutationResult::invalid_path:
        (void)output.format(",\"Error\":\"Invalid path\",\"Code\":%d}}", filesystem::last_error());
        return CommandStatus::failed;
    case filesystem::MutationResult::failed:
        (void)output.format(",\"Error\":\"Remove failed\",\"Code\":%d}}", filesystem::last_error());
        return CommandStatus::failed;
    }
    return CommandStatus::failed;
}

/**
 * @brief Handles renaming or moving one filesystem entry.
 */
CommandStatus mv_command(const CommandRequest& request, CommandOutput& output) {
    if (request.has_index || request.is_query()) {
        return CommandStatus::invalid;
    }

    std::string_view arguments[2]{};
    if (parse_arguments(request.payload, arguments, 2) != ArgumentResult::parsed) {
        return CommandStatus::invalid;
    }

    const auto result = filesystem::rename(arguments[0], arguments[1]);
    if (!output.write("{\"mv\":{\"Source\":") || !output.json_string(arguments[0]) ||
        !output.write(",\"Destination\":") || !output.json_string(arguments[1])) {
        return CommandStatus::failed;
    }
    switch (result) {
    case filesystem::MutationResult::succeeded:
        return output.write(",\"Result\":\"Renamed\"}}") ? CommandStatus::handled
                                                         : CommandStatus::failed;
    case filesystem::MutationResult::unavailable:
        (void)output.write(",\"Error\":\"Filesystem unavailable\"}}");
        return CommandStatus::failed;
    case filesystem::MutationResult::invalid_path:
        (void)output.format(",\"Error\":\"Invalid path\",\"Code\":%d}}", filesystem::last_error());
        return CommandStatus::failed;
    case filesystem::MutationResult::failed:
        (void)output.format(",\"Error\":\"Rename failed\",\"Code\":%d}}", filesystem::last_error());
        return CommandStatus::failed;
    }
    return CommandStatus::failed;
}

constexpr CommandDefinition kCommands[]{
    {"df", df_command},
    {"ls", ls_command},
    {"rm", rm_command},
    {"mv", mv_command},
};

} // namespace

/**
 * @brief Dispatches a request through the fixed filesystem command table.
 */
CommandStatus dispatch_filesystem(const CommandRequest& request, CommandOutput& output) {
    for (const auto& command : kCommands) {
        if (support::ascii_iequals(request.name, command.name)) {
            return command.handler(request, output);
        }
    }
    return CommandStatus::unknown;
}

} // namespace newmota::commands
