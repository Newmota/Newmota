/*
  audio_webrtc.h - Microphone audio for the CSI camera's WebRTC track in Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_webrtc.h
 * @brief Opens the I2S microphone while a WebRTC peer streams, Tasmota's WcAudio.
 */
#pragma once

namespace newmota::camera {
/** @brief Starts or stops the audio track with the WebRTC peer; application task only. */
void poll_audio();
/** @brief Stops the audio worker and releases the microphone. */
void stop_audio();
} // namespace newmota::camera
