/*
  audio_store.h - Persistent I2SConfig profiles in the audio driver's own file

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_store.h
 * @brief Loads or creates `/.newmota/audio.bin` and saves the profiles to it.
 *
 * The audio driver keeps its settings out of the shared settings record, so Safeboot never
 * sees them. Pins are never stored; they always come from the hardware template.
 */
#pragma once
#include "core/storage/driver_settings_file.h"
#include "drivers/audio/audio_settings.h"

namespace newmota::audio {
/** @brief Reads the audio settings file, writing a default one when it is missing.
 * Call once at startup. The profiles stay readable through stored_settings() whatever the
 * outcome; a failed filesystem leaves the defaults in effect for this boot.
 */
DriverSettingsState initialize_store();
/** @brief Returns the profiles read at startup or last saved. */
const AudioSettings& stored_settings();
/** @brief Persists @p settings; the cached copy changes only after a successful write. */
[[nodiscard]] bool store_settings(const AudioSettings& settings);
} // namespace newmota::audio
