/*
  audio_codecs.h - Carried Tasmota audio codec providers

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_codecs.h
 * @brief Incremental codecs selected independently of native I2S transport.
 */
#pragma once
#include "drivers/audio/audio_session.h"

namespace newmota::audio {
/** @brief Opens a compiled codec, or returns Unsupported without acquiring resources. */
Result open_codec(const Config&, bool encode, Stage&);

namespace detail {
/** @brief Renders `config.location` with carried SAM when opened; decode-only. */
Result open_sam(const Config&, bool encode, Stage&);
/** @brief Opens the square-wave generator; decode-only, one at a time. */
Result open_tone(const Config&, bool encode, Stage&);
/** @brief Opens carried fixed-point Opus with bounded Ogg/WebM framing. */
Result open_opus(const Config&, bool encode, Stage&);
} // namespace detail
} // namespace newmota::audio
