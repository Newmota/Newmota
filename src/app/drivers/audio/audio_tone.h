/*
  audio_tone.h - Square-wave tone generator that Berry retunes while it plays

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_tone.h
 * @brief The `text` source with the `tone` codec plays until stopped; set_tone() changes it.
 */
#pragma once
#include "drivers/audio/audio_session.h"

namespace newmota::audio {
/** @brief Highest tone, the Nyquist limit of the generator's fixed output rate. */
inline constexpr std::uint32_t kToneMaximumHz = 24000;

/** @brief Retunes the open generator: 0 Hz is silence, volume 0..100 % of full scale.
 * The change is heard after the queued playback PCM, a constant delay of about 0.1 s.
 */
Result set_tone(std::uint32_t hz, std::uint32_t volume);
} // namespace newmota::audio
