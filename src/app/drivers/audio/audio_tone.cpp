/*
  audio_tone.cpp - Square-wave tone generator that Berry retunes while it plays

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_tone.cpp
 * @brief Produces stereo 16-bit PCM until the session finishes; silent at 0 Hz.
 */
#include "drivers/audio/audio_tone.h"
#include "drivers/audio/audio_codecs.h"

namespace newmota::audio {
namespace {
/** @brief Stereo at 48 kHz keeps the 16 KiB playback ring, and so the delay, near 85 ms. */
constexpr std::uint32_t kToneSampleRateHz = 48000;
constexpr std::uint8_t kToneChannels = 2;
constexpr std::size_t kToneFrameBytes = kToneChannels * sizeof(std::int16_t);
constexpr std::int32_t kFullScale = 32767;

/** @brief One generator; the stage and set_tone() both run on the application loop. */
struct Tone {
    bool open{};
    std::uint32_t phase{}, step{};
    std::int16_t amplitude{};
};

Tone tone{};

Step tone_process(
    void*, const std::uint8_t*, std::size_t, bool eof, std::uint8_t* output, std::size_t capacity) {
    Step step{};
    step.sample_rate = kToneSampleRateHz;
    step.channels = kToneChannels;
    if (eof) {
        step.finished = true;
        return step;
    }
    const auto frames = capacity / kToneFrameBytes;
    for (std::size_t i = 0; i < frames; ++i) {
        std::int16_t value = 0;
        if (tone.step) {
            value = (tone.phase & 0x80000000U) ? static_cast<std::int16_t>(-tone.amplitude)
                                               : tone.amplitude;
            tone.phase += tone.step;
        }
        const auto low = static_cast<std::uint8_t>(value);
        const auto high = static_cast<std::uint8_t>(static_cast<std::uint16_t>(value) >> 8U);
        auto* frame = output + i * kToneFrameBytes;
        frame[0] = frame[2] = low;
        frame[1] = frame[3] = high;
    }
    step.produced = frames * kToneFrameBytes;
    return step;
}

void tone_close(void*) {
    tone = {};
}
} // namespace

Result set_tone(std::uint32_t hz, std::uint32_t volume) {
    if (!tone.open)
        return Result::invalid_argument;
    if (hz > kToneMaximumHz || volume > 100U)
        return Result::invalid_argument;
    // The phase carries over, so a new pitch starts without a click.
    tone.step = static_cast<std::uint32_t>((std::uint64_t{hz} << 32U) / kToneSampleRateHz);
    tone.amplitude =
        static_cast<std::int16_t>(kFullScale * static_cast<std::int32_t>(volume) / 100);
    return Result::success;
}

namespace detail {
Result open_tone(const Config&, bool encode, Stage& stage) {
    stage = {};
    if (encode)
        return Result::invalid_argument;
    if (tone.open)
        return Result::busy;
    tone = {};
    tone.open = true;
    stage = {&tone, tone_process, nullptr, tone_close};
    return Result::success;
}
} // namespace detail
} // namespace newmota::audio
