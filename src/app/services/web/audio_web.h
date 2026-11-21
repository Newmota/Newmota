/*
  audio_web.h - browser view of the command audio session

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file audio_web.h
 * @brief JSON contract for the Home audio player card.
 *
 * The card reads the command-owned session that the I2S commands start, so console, MQTT and
 * the page see one player. Controls go through `/api/command` (`I2SPlay`, `I2SLoop`, `I2SWr`,
 * `I2SPause`, `I2SStop`, `I2SGain`); this route only reports.
 */
#pragma once

namespace newmota {
class CommandOutput;

namespace audio_web {

/** @brief Writes the session state, its source, gain, ICY metadata and the compiled codecs.
 * Application task only.
 */
[[nodiscard]] bool write_player(CommandOutput& output);

} // namespace audio_web
} // namespace newmota
