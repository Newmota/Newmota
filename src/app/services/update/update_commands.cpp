/*
  update_commands.cpp - Tasmota-compatible URL update commands

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/update/update_commands.h"

#include "services/update/url_update_service.h"
#include "newmota/platform/platform.h"
#include "support/text.h"

namespace newmota::commands {

/** @brief Writes one bounded progress response for the Upgrade query. */
[[nodiscard]] CommandStatus dispatch_url_update(
    const CommandRequest& request, UrlUpdateService& update, CommandOutput& output) {
    if (!support::ascii_iequals(request.name, "upgrade")) {
        return CommandStatus::unknown;
    }

    bool start = false;
    if (request.has_index) {
        if (request.index != 1U || request.has_payload) {
            return CommandStatus::invalid;
        }
        start = true;
    } else if (request.is_query()) {
        return output.format("{\"Upgrade\":\"%s\",\"Progress\":%u,"
                             "\"Received\":%lu,\"Total\":%lu,\"Error\":",
                   update.state_name(), update.progress_percent(),
                   static_cast<unsigned long>(update.received_bytes()),
                   static_cast<unsigned long>(update.expected_bytes())) &&
                       output.json_string(update.error_message()) && output.write("}")
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    } else if (request.payload == "1") {
        start = true;
    } else {
        return CommandStatus::invalid;
    }

    if (!start)
        return CommandStatus::invalid;
    switch (update.request(platform::monotonic_milliseconds())) {
    case UrlUpdateRequestResult::started:
        return output.write("{\"Upgrade\":\"Started\"}") ? CommandStatus::handled
                                                         : CommandStatus::failed;
    case UrlUpdateRequestResult::handoff:
        return output.write("{\"Upgrade\":\"Switching to Safeboot\"}") ? CommandStatus::handled
                                                                       : CommandStatus::failed;
    case UrlUpdateRequestResult::busy:
        (void)output.write("{\"Upgrade\":\"Update already active\"}");
        return CommandStatus::failed;
    case UrlUpdateRequestResult::no_url:
        (void)output.write("{\"Upgrade\":\"OtaUrl is not configured\"}");
        return CommandStatus::failed;
    case UrlUpdateRequestResult::invalid_url:
        return output.format("{\"Upgrade\":\"%s\"}", update.error_message())
                   ? CommandStatus::invalid
                   : CommandStatus::failed;
    case UrlUpdateRequestResult::unavailable:
        (void)output.write("{\"Upgrade\":\"Native update path unavailable\"}");
        return CommandStatus::failed;
    case UrlUpdateRequestResult::failed:
        return output.format("{\"Upgrade\":\"%s\"}", update.error_message())
                   ? CommandStatus::failed
                   : CommandStatus::failed;
    }
    return CommandStatus::failed;
}

} // namespace newmota::commands
