/*
  audio_level.h - Microphone level meter and level histogram

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_level.h
 * @brief Native analyzer endpoint measuring RMS level, peak, Leq, percentiles, and a histogram.
 */
#pragma once
#include "drivers/audio/audio_session.h"
#include <array>

namespace newmota::audio {
/** @brief Number of coarse histogram buckets reported by level_histogram(). */
inline constexpr std::size_t kLevelHistogramBuckets = 24;
/** @brief Width in dB of one coarse histogram bucket. */
inline constexpr int kLevelHistogramStepDb = 5;

/** @brief Measurements since the last reset. Levels are tenths of a dB, calibration offset
 * included. A level is the RMS of one 125 ms window relative to full scale; with the default
 * offset of zero it is dBFS.
 */
struct LevelSnapshot {
    bool running{}, alarm_enabled{}, alarm_active{};
    std::uint32_t windows{}, clipped{}, alarm_count{};
    std::int32_t level{}, peak{}, leq{}, min{}, max{}, l10{}, l50{}, l90{};
    std::int32_t offset_db{}, alarm_db{};
};

/** @brief Registers the analyzer as a native endpoint named "level" (sink `custom`). */
void initialize_level();
/** @brief Starts the analyzer on the command input profile; a running analyzer is kept. */
Result level_start();
/** @brief Stops the analyzer session; measurements stay readable until reset or restart. */
Result level_stop();
/** @brief Clears statistics; the calibration offset and alarm threshold are retained. */
void level_reset();
/** @brief Sets the dB added to every reported level (SPL at full scale for a calibrated mic). */
void level_set_offset(std::int32_t db);
/** @brief Sets or disables the alarm; it clears 3 dB below the threshold. */
void level_set_alarm(bool enabled, std::int32_t db);
LevelSnapshot level_snapshot();
/** @brief Fills 5 dB buckets of window counts; returns the lowest bucket's start in dB. */
std::int32_t level_histogram(std::array<std::uint32_t, kLevelHistogramBuckets>& counts);
} // namespace newmota::audio
