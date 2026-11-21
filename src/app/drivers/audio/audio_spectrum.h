/*
  audio_spectrum.h - Microphone third-octave spectrum analyzer

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_spectrum.h
 * @brief Native analyzer endpoint reporting FFT-based third-octave band levels.
 */
#pragma once
#include "drivers/audio/audio_session.h"
#include <array>

namespace newmota::audio {
/** @brief Most third-octave bands reported (25 Hz to 20 kHz). */
inline constexpr std::size_t kSpectrumMaximumBands = 30;

/** @brief Which per-band series spectrum_series() returns. */
enum class SpectrumSeries : std::uint8_t { latest, average, hold };

/** @brief Frame counts and the dominant tone of the latest frame. */
struct SpectrumSnapshot {
    bool running{};
    std::uint32_t frames{};
    std::uint32_t fft_size{};
    std::uint32_t bands{};
    /** @brief Frequency of the strongest bin of the latest frame, in Hz. */
    std::uint32_t peak_hz{};
    /** @brief Level of that bin in tenths of a dB relative to full scale. */
    std::int32_t peak_level{};
};

/** @brief Registers the analyzer as a native endpoint named "spectrum" (sink `custom`). */
void initialize_spectrum();
/** @brief Starts the analyzer on the command input profile; a running analyzer is kept. */
Result spectrum_start();
/** @brief Stops the analyzer session; measurements stay readable until reset or restart. */
Result spectrum_stop();
/** @brief Clears the average and the maximum hold. */
void spectrum_reset();
SpectrumSnapshot spectrum_snapshot();
/** @brief Copies band centers (Hz) and levels (tenths of a dB relative to full scale).
 * @return Number of bands written; zero before the first frame.
 */
std::size_t spectrum_series(SpectrumSeries series,
    std::array<std::uint32_t, kSpectrumMaximumBands>& centers,
    std::array<std::int32_t, kSpectrumMaximumBands>& levels);
} // namespace newmota::audio
