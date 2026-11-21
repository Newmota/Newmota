/*
  state_text.cpp - Shared state-word presentation and command support

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  CmndStateText/GetStateText reference: Tasmota support_command.ino/support_tasmota.ino,
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
*/
/** @file state_text.cpp
 * @brief Bounded durable state-word aliases with escaped command output.
 */
#include "services/settings/state_text.h"
#include "services/settings/settings.h"
#include "core/command/command.h"
#include "support/text.h"

namespace newmota {
namespace {
const Settings* owner{};
}

void bind_state_text(const Settings& settings) {
    owner = &settings;
}

std::string_view state_text(unsigned index) {
    if (owner)
        return owner->state_text(index);
    switch (index) {
    case 0:
        return "OFF";
    case 2:
        return "TOGGLE";
    case 3:
        return "HOLD";
    default:
        return "ON";
    }
}

namespace commands {
CommandStatus dispatch_state_text(
    const CommandRequest& request, Settings& settings, CommandOutput& output) {
    if (!support::ascii_iequals(request.name, "StateText"))
        return CommandStatus::unknown;
    if (request.has_index && (!request.index || request.index > 4))
        return CommandStatus::invalid;
    if (request.has_index && !request.is_query()) {
        const auto result = settings.set_state_text(request.index - 1, request.payload);
        if (result == SettingsUpdateResult::failed)
            return CommandStatus::failed;
        if (result == SettingsUpdateResult::invalid)
            return CommandStatus::invalid;
    }
    if (!output.write("{"))
        return CommandStatus::failed;
    for (unsigned index = 0; index < 4; ++index) {
        if (request.has_index && index + 1 != request.index)
            continue;
        if ((!request.has_index && index && !output.write(",")) ||
            !output.format("\"StateText%u\":", index + 1) ||
            !output.json_string(settings.state_text(index)))
            return CommandStatus::failed;
    }
    return output.write("}") ? CommandStatus::handled : CommandStatus::failed;
}
} // namespace commands
} // namespace newmota
