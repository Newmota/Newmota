/*
  audio_sam.cpp - Carried SAM speech synthesis as a text-to-PCM stage

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_sam.cpp
 * @brief Renders the whole utterance when opened, then streams it as 16-bit PCM.
 */
#include "drivers/audio/audio_codecs.h"
#include "newmota/platform/platform.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <new>
extern "C" {
#include "reciter.h"
#include "sam.h"
#include "SamData.h"
}

SamData* samdata{};

namespace newmota::audio::detail {
namespace {
/** @brief SAM's fixed output: unsigned 8-bit mono at 22.05 kHz. */
constexpr std::uint32_t kSamSampleRateHz = 22050;
constexpr std::size_t kSamMaximumTextBytes = 254;
constexpr std::size_t kSamInputBytes = 256;
constexpr std::uint8_t kUnsignedPcmCenter = 128;
constexpr std::size_t kChunkBytes = 4096;
/** @brief About 10 s of speech, the bound the carried synthesizer documents. */
constexpr std::size_t kMaximumChunks = 56;
/** @brief Sleep once per this many rendered samples so the idle task and watchdog run. */
constexpr std::size_t kRenderYieldSamples = 4096;

/** @brief Rendered utterance held as separately allocated chunks; no large contiguous block. */
struct Sam {
    std::array<std::uint8_t*, kMaximumChunks> chunks{};
    std::size_t size{}, position{};
    bool truncated{};

    ~Sam() {
        for (auto* chunk : chunks)
            delete[] chunk;
    }

    void append(std::uint8_t sample) {
        const auto index = size / kChunkBytes;
        if (index >= chunks.size()) {
            truncated = true;
            return;
        }
        if (!chunks[index]) {
            chunks[index] = new (std::nothrow) std::uint8_t[kChunkBytes];
            if (!chunks[index]) {
                truncated = true;
                return;
            }
        }
        chunks[index][size % kChunkBytes] = sample;
        if (++size % kRenderYieldSamples == 0U)
            platform::sleep_for_milliseconds(1);
    }
};

void emit(void* context, unsigned char sample) {
    auto& sam = *static_cast<Sam*>(context);
    if (!sam.truncated)
        sam.append(sample);
}

Step sam_process(void* context, const std::uint8_t*, std::size_t, bool, std::uint8_t* output,
    std::size_t capacity) {
    auto& sam = *static_cast<Sam*>(context);
    Step step{};
    const auto count = std::min(sam.size - sam.position, capacity / sizeof(std::int16_t));
    for (std::size_t i = 0; i < count; ++i, ++sam.position) {
        const auto unsigned_sample =
            sam.chunks[sam.position / kChunkBytes][sam.position % kChunkBytes];
        const auto value = static_cast<std::int16_t>(
            (static_cast<int>(unsigned_sample) - kUnsignedPcmCenter) * 256);
        output[i * 2U] = static_cast<std::uint8_t>(value);
        output[i * 2U + 1U] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(value) >> 8U);
    }
    step.produced = count * sizeof(std::int16_t);
    step.sample_rate = kSamSampleRateHz;
    step.channels = 1;
    step.finished = sam.position == sam.size;
    return step;
}

void sam_close(void* context) {
    delete static_cast<Sam*>(context);
}
} // namespace

Result open_sam(const Config& config, bool encode, Stage& stage) {
    stage = {};
    const std::string_view text{config.location.data()};
    if (encode || text.empty() || text.size() > kSamMaximumTextBytes)
        return Result::invalid_argument;
    std::array<char, kSamInputBytes> input{};
    for (std::size_t i = 0; i < text.size(); ++i)
        input[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
    input[text.size()] = '[';
    auto* sam = new (std::nothrow) Sam();
    // The carried synthesizer keeps its working tables behind one global pointer.
    samdata = new (std::nothrow) SamData();
    if (!sam || !samdata) {
        delete sam;
        delete samdata;
        samdata = nullptr;
        return Result::no_memory;
    }
    EnableSingmode(0);
    Result result = Result::success;
    if (!TextToPhonemes(input.data()))
        result = Result::invalid_argument;
    else {
        SetInput(input.data());
        if (!SAMMain(emit, sam))
            result = Result::codec_error;
    }
    delete samdata;
    samdata = nullptr;
    if (result == Result::success && sam->truncated)
        result = Result::no_memory;
    if (result == Result::success && !sam->size)
        result = Result::codec_error;
    if (result != Result::success) {
        delete sam;
        return result;
    }
    stage = {sam, sam_process, nullptr, sam_close};
    return Result::success;
}
} // namespace newmota::audio::detail
