/*
  audio_commands.h - Native audio command dispatch

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_commands.h
 * @brief Shared audio commands over the command-owned native session.
 */
#pragma once
#include "core/command/command.h"

namespace newmota {
class TimeService;
struct AudioSettings;
} // namespace newmota

namespace newmota::commands {
/** @brief Dispatches I2SWr, I2SPlay, I2SRec, I2SStream and I2SLoop. */
CommandStatus dispatch_audio(const CommandRequest&, CommandOutput&);
/** @brief Overlays the stored I2SConfig profiles on the template-derived command defaults.
 * Call after the template pins have been resolved and the audio store has been initialized.
 */
void apply_audio_settings(const AudioSettings&);
/** @brief Handles I2STime: speaks the local 12-hour time with SAM.
 * @return `unknown` for any other command, so the caller can fall through.
 */
CommandStatus dispatch_audio_say_time(
    const CommandRequest&, const TimeService& time, CommandOutput&);
} // namespace newmota::commands
