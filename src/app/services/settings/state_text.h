/*
  state_text.h - Shared state-word presentation and command support

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  CmndStateText/GetStateText reference: Tasmota support_command.ino/support_tasmota.ino,
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
*/
/** @file state_text.h
 * @brief Read-only state aliases with feature-independent default presentation.
 */
#pragma once
#include "newmota_features.h"
#include <string_view>

namespace newmota {
class Settings;
struct CommandRequest;
class CommandOutput;
enum class CommandStatus : unsigned char;
#if USE_STATE_TEXT
/** @brief Binds the application-lifetime settings owner after persistent settings load.
 * @param settings Owner retained for synchronous reads; no alias copies or sample mutation.
 */
void bind_state_text(const Settings& settings);
/** @brief Returns OFF/ON/TOGGLE/HOLD alias; invalid indexes fall back to ON.
 * @param index Zero-based state index.
 * @return Alias valid until the next Settings mutation.
 */
std::string_view state_text(unsigned index);

namespace commands {
/** @brief Dispatches indexed StateText setters/queries and unindexed all-state queries. */
CommandStatus dispatch_state_text(const CommandRequest&, Settings&, CommandOutput&);
} // namespace commands
#else
/** @brief Returns source default words when optional alias support is excluded. */
inline std::string_view state_text(unsigned index) {
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
#endif
} // namespace newmota
