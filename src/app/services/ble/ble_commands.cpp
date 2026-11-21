/*
  ble_commands.cpp - BLE scan commands

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ble_commands.cpp
 * @brief Scan controls shared by every BLE backend. Nothing is saved: a script such as
 * autoexec.be repeats them after boot.
 */
#include "services/ble/ble_commands.h"

#include "newmota/platform/ble.h"
#include "support/number.h"
#include "support/text.h"

namespace newmota::commands {
namespace {
/** @brief Converts whole milliseconds to 0.625 ms units, rounded. */
constexpr std::uint16_t units(std::uint32_t milliseconds) {
    return static_cast<std::uint16_t>((milliseconds * 8U + 2U) / 5U);
}

/** @brief Converts 0.625 ms units to whole milliseconds, rounded. */
constexpr unsigned milliseconds(std::uint16_t units) {
    return (units * 5U + 4U) / 8U;
}

[[nodiscard]] int nibble(char character) {
    if (character >= '0' && character <= '9')
        return character - '0';
    character = support::ascii_lower(character);
    return character >= 'a' && character <= 'f' ? character - 'a' + 10 : -1;
}

/** @brief Parses twelve hex digits, optionally separated by colons, into display order. */
[[nodiscard]] bool parse_address(std::string_view text, std::uint8_t* address) {
    unsigned digits = 0;
    for (const char character : text) {
        if (character == ':')
            continue;
        const int value = nibble(character);
        if (value < 0 || digits == 12)
            return false;
        auto& byte = address[digits / 2];
        byte = static_cast<std::uint8_t>(digits % 2 == 0 ? value << 4 : byte | value);
        ++digits;
    }
    return digits == 12;
}

CommandStatus active_command(const CommandRequest& request, CommandOutput& output) {
    auto parameters = platform::ble_scan_parameters();
    if (!request.is_query()) {
        std::uint32_t value = 0;
        if (!support::parse_unsigned_decimal(request.payload, 1, value))
            return CommandStatus::invalid;
        parameters.active = value != 0;
        if (!platform::ble_scan_configure(parameters))
            return CommandStatus::failed;
    }
    return output.format("{\"BLEActive\":%u}", parameters.active ? 1U : 0U) ? CommandStatus::handled
                                                                            : CommandStatus::failed;
}

CommandStatus scan_command(const CommandRequest& request, CommandOutput& output) {
    auto parameters = platform::ble_scan_parameters();
    if (!request.is_query()) {
        const auto comma = request.payload.find(',');
        if (comma == std::string_view::npos)
            return CommandStatus::invalid;
        std::uint32_t interval = 0, window = 0;
        if (!support::parse_unsigned_decimal(
                support::trim_ascii(request.payload.substr(0, comma)), 640, interval) ||
            !support::parse_unsigned_decimal(
                support::trim_ascii(request.payload.substr(comma + 1)), 640, window))
            return CommandStatus::invalid;
        parameters.interval = units(interval);
        parameters.window = units(window);
        if (!platform::ble_scan_valid(parameters))
            return CommandStatus::invalid;
        if (!platform::ble_scan_configure(parameters))
            return CommandStatus::failed;
    }
    return output.format("{\"BLEScan\":{\"Interval\":%u,\"Window\":%u}}",
               milliseconds(parameters.interval), milliseconds(parameters.window))
               ? CommandStatus::handled
               : CommandStatus::failed;
}

CommandStatus watch_command(const CommandRequest& request, CommandOutput& output) {
    if (request.payload == "0") {
        platform::ble_scan_unwatch();
    } else if (!request.is_query()) {
        const auto comma = request.payload.find(',');
        std::uint8_t address[6];
        std::uint32_t type = 0;
        if (!parse_address(support::trim_ascii(request.payload.substr(0, comma)), address) ||
            (comma != std::string_view::npos &&
                !support::parse_unsigned_decimal(
                    support::trim_ascii(request.payload.substr(comma + 1)), 3, type)))
            return CommandStatus::invalid;
        if (!platform::ble_scan_watch(address, static_cast<std::uint8_t>(type))) {
            (void)output.write("{\"BLEWatch\":\"Failed\"}");
            return CommandStatus::failed;
        }
    }
    bool okay = output.write("{\"BLEWatch\":[");
    std::uint8_t address[6], type = 0;
    for (std::size_t i = 0; okay && platform::ble_scan_watched(i, address, type); ++i)
        okay =
            output.format("%s{\"MAC\":\"%02X%02X%02X%02X%02X%02X\",\"Type\":%u}", i == 0 ? "" : ",",
                address[0], address[1], address[2], address[3], address[4], address[5], type);
    return okay && output.write("]}") ? CommandStatus::handled : CommandStatus::failed;
}
} // namespace

CommandStatus dispatch_ble(const CommandRequest& request, CommandOutput& output) {
    CommandStatus (*handler)(const CommandRequest&, CommandOutput&) = nullptr;
    if (support::ascii_iequals(request.name, "BLEActive"))
        handler = active_command;
    else if (support::ascii_iequals(request.name, "BLEScan"))
        handler = scan_command;
    else if (support::ascii_iequals(request.name, "BLEWatch"))
        handler = watch_command;
    if (handler == nullptr)
        return CommandStatus::unknown;
    return request.has_index ? CommandStatus::invalid : handler(request, output);
}
} // namespace newmota::commands
