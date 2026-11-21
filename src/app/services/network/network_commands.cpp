/*
  network_commands.cpp - read-only network information commands for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/network/network_commands.h"

#include "newmota/platform/network.h"
#include "support/text.h"

#include <cstddef>
#include <cstdint>

namespace newmota::commands {
namespace {

/** @brief Treats an unassigned SDK address as absent for AP fallback. */
[[nodiscard]] bool has_address(const platform::NetworkInfo& info) {
    const std::string_view address{info.address};
    return !address.empty() && address != "0.0.0.0";
}

/** @brief Converts one fixed platform buffer into a bounded text view. */
template <std::size_t N> [[nodiscard]] std::string_view bounded_text(const char (&value)[N]) {
    std::size_t length = 0U;
    while (length < N && value[length] != '\0') {
        ++length;
    }
    return {value, length};
}

/** @brief Writes one named JSON network field. */
[[nodiscard]] bool write_field(
    CommandOutput& output, const char* const name, const std::string_view value) {
    return output.format("{\"%s\":", name) && output.json_string(value) && output.write("}");
}

/** @brief Writes all five Tasmota-compatible IP address fields. */
[[nodiscard]] bool write_all_addresses(CommandOutput& output, const platform::NetworkInfo& info) {
    return output.write("{\"IPAddress1\":") && output.json_string(bounded_text(info.address)) &&
           output.write(",\"IPAddress2\":") && output.json_string(bounded_text(info.gateway)) &&
           output.write(",\"IPAddress3\":") && output.json_string(bounded_text(info.subnet)) &&
           output.write(",\"IPAddress4\":") && output.json_string(bounded_text(info.dns1)) &&
           output.write(",\"IPAddress5\":") && output.json_string(bounded_text(info.dns2)) &&
           output.write("}");
}

/** @brief Writes one indexed Tasmota IP address field. */
[[nodiscard]] bool write_address(
    CommandOutput& output, const std::uint16_t index, const platform::NetworkInfo& info) {
    std::string_view value;
    switch (index) {
    case 1:
        value = bounded_text(info.address);
        break;
    case 2:
        value = bounded_text(info.gateway);
        break;
    case 3:
        value = bounded_text(info.subnet);
        break;
    case 4:
        value = bounded_text(info.dns1);
        break;
    case 5:
        value = bounded_text(info.dns2);
        break;
    default:
        return false;
    }
    return output.format("{\"IPAddress%u\":", static_cast<unsigned int>(index)) &&
           output.json_string(value) && output.write("}");
}

} // namespace

/** @brief Dispatches read-only network identity and address queries. */
CommandStatus dispatch_network_queries(const CommandRequest& request, CommandOutput& output) {
    const bool hostname = support::ascii_iequals(request.name, "hostname");
    const bool ip_address = support::ascii_iequals(request.name, "ipaddress");
    if (!hostname && !ip_address) {
        return CommandStatus::unknown;
    }
    if (!request.is_query()) {
        return CommandStatus::invalid;
    }

    auto info = platform::network_info();
    if (!has_address(info)) {
        info = platform::access_point_info();
    }
    if (hostname) {
        if (request.has_index) {
            return CommandStatus::invalid;
        }
        return write_field(output, "Hostname", bounded_text(info.hostname)) ? CommandStatus::handled
                                                                            : CommandStatus::failed;
    }

    if (request.has_index && (request.index == 0U || request.index > 5U)) {
        return CommandStatus::invalid;
    }
    const bool written = request.has_index ? write_address(output, request.index, info)
                                           : write_all_addresses(output, info);
    return written ? CommandStatus::handled : CommandStatus::failed;
}

} // namespace newmota::commands
