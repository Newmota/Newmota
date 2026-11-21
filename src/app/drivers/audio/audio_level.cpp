/*
  audio_level.cpp - Microphone level meter and level histogram

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_level.cpp
 * @brief Integer per-sample accumulation with one floating-point step per 125 ms window.
 */
#include "drivers/audio/audio_level.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>

namespace newmota::audio {
namespace {
constexpr std::string_view kEndpointName = "level";
/** @brief Window length of a "fast" sound level meter. */
constexpr std::uint32_t kWindowMilliseconds = 125;
constexpr std::uint32_t kMillisecondsPerSecond = 1000;
constexpr int kFloorDb = -120;
/** @brief One-dB bins covering [-120, 0) dB relative to full scale. */
constexpr std::size_t kBins = 120;
constexpr int kAlarmHysteresisDb = 3;
constexpr int kTenths = 10;
/** @brief Samples are normalized to 24 bits: 16-bit values shift up, 32-bit containers down. */
constexpr unsigned kSampleBits = 24;
constexpr std::int32_t kFullScale = 1 << (kSampleBits - 1U);
constexpr double kFullScaleSquared = static_cast<double>(kFullScale) * kFullScale;
/** @brief A window clips when a sample is within one 16-bit step of full scale. */
constexpr std::int32_t kClipThreshold = kFullScale - (1 << (kSampleBits - 16U));
constexpr unsigned kByteShift = 8;
constexpr std::size_t kPcm16Bytes = 2;
constexpr std::size_t kPcm32Bytes = 4;

/** @brief Calibration and alarm settings; they survive reset and restart. */
struct Settings {
    std::int32_t offset_db{};
    bool alarm_enabled{};
    std::int32_t alarm_db{};
};

struct Analyzer {
    bool open{}, wide{};
    std::size_t frame_bytes{}, window_samples{}, filled{};
    unsigned channels{};
    // Accumulators for the current window.
    std::uint64_t sum_squares{};
    std::int64_t sum{};
    std::int32_t min{}, max{};
    // Statistics.
    std::array<std::uint32_t, kBins> bins{};
    std::uint32_t windows{}, clipped{}, alarm_count{};
    double energy{};
    std::int32_t level{}, peak{}, min_level{}, max_level{};
    bool alarm_active{};
};

Analyzer analyzer;
Settings settings;
Handle session{};

constexpr std::int32_t kEmptyMin = std::numeric_limits<std::int32_t>::max();
constexpr std::int32_t kEmptyMax = std::numeric_limits<std::int32_t>::min();

void clear_window(Analyzer& a) {
    a.sum_squares = 0;
    a.sum = 0;
    a.min = kEmptyMin;
    a.max = kEmptyMax;
    a.filled = 0;
}

void clear_statistics(Analyzer& a) {
    a.bins = {};
    a.windows = a.clipped = a.alarm_count = 0;
    a.energy = 0;
    a.level = a.peak = a.min_level = a.max_level = 0;
    a.alarm_active = false;
    clear_window(a);
}

std::int32_t tenths(double db) {
    return static_cast<std::int32_t>(std::lround(std::clamp(db, double{kFloorDb}, 0.0) * kTenths));
}

void finish_window(Analyzer& a) {
    const auto count = static_cast<double>(a.filled);
    const auto mean = static_cast<double>(a.sum) / count;
    // The window mean removes a microphone's DC offset from both RMS and peak.
    const auto mean_square =
        std::max(static_cast<double>(a.sum_squares) / count - mean * mean, 0.0);
    const auto rms_db =
        mean_square > 0 ? 10.0 * std::log10(mean_square / kFullScaleSquared) : double{kFloorDb};
    const auto swing = std::max(a.max - mean, mean - a.min);
    const auto peak_db = swing > 0 ? 20.0 * std::log10(swing / kFullScale) : double{kFloorDb};
    a.level = tenths(rms_db);
    a.peak = tenths(peak_db);
    const auto bin =
        std::clamp(static_cast<int>(std::floor(rms_db)) - kFloorDb, 0, static_cast<int>(kBins) - 1);
    ++a.bins[static_cast<std::size_t>(bin)];
    a.energy += mean_square / kFullScaleSquared;
    if (!a.windows || a.level < a.min_level)
        a.min_level = a.level;
    if (!a.windows || a.level > a.max_level)
        a.max_level = a.level;
    ++a.windows;
    if (a.max >= kClipThreshold || a.min <= -kClipThreshold)
        ++a.clipped;
    if (settings.alarm_enabled) {
        const auto shown = a.level + settings.offset_db * kTenths;
        if (!a.alarm_active && shown >= settings.alarm_db * kTenths) {
            a.alarm_active = true;
            ++a.alarm_count;
        } else if (a.alarm_active && shown < (settings.alarm_db - kAlarmHysteresisDb) * kTenths)
            a.alarm_active = false;
    } else
        a.alarm_active = false;
    clear_window(a);
}

/** @brief Adds `count` little-endian samples, widened or narrowed to the 24-bit scale. */
template <typename Load>
void accumulate(
    Analyzer& a, const std::uint8_t* bytes, std::size_t count, std::size_t stride, Load load) {
    auto sum_squares = a.sum_squares;
    auto sum = a.sum;
    auto low = a.min;
    auto high = a.max;
    for (std::size_t i = 0; i < count; ++i) {
        const std::int32_t sample = load(bytes + i * stride);
        sum_squares += static_cast<std::uint64_t>(static_cast<std::int64_t>(sample) * sample);
        sum += sample;
        low = std::min(low, sample);
        high = std::max(high, sample);
    }
    a.sum_squares = sum_squares;
    a.sum = sum;
    a.min = low;
    a.max = high;
}

Io level_write(void* context, const std::uint8_t* bytes, std::size_t size) {
    auto& a = *static_cast<Analyzer*>(context);
    const auto frames = size / a.frame_bytes;
    auto samples = frames * a.channels;
    const auto stride = a.wide ? kPcm32Bytes : kPcm16Bytes;
    while (samples) {
        const auto count = std::min(samples, a.window_samples - a.filled);
        if (a.wide)
            accumulate(a, bytes, count, stride, [](const std::uint8_t* p) {
                std::int32_t value;
                std::memcpy(&value, p, sizeof(value));
                return value >> kByteShift;
            });
        else
            accumulate(a, bytes, count, stride, [](const std::uint8_t* p) {
                std::int16_t value;
                std::memcpy(&value, p, sizeof(value));
                return static_cast<std::int32_t>(value) * (1 << kByteShift);
            });
        bytes += count * stride;
        samples -= count;
        a.filled += count;
        if (a.filled == a.window_samples)
            finish_window(a);
    }
    return {Result::success, frames * a.frame_bytes};
}

void level_close(void* context) {
    static_cast<Analyzer*>(context)->open = false;
}

Result endpoint(const Config& config, bool input, Endpoint& opened) {
    if (input || config.sink != Sink::custom ||
        std::string_view{config.location.data()} != kEndpointName)
        return Result::unsupported;
    if (analyzer.open)
        return Result::busy;
    const auto& in = config.input;
    if (in.mode == platform::i2s::Mode::pdm && !in.pdm_pcm)
        return Result::unsupported;
    const auto window_frames =
        std::size_t{in.sample_rate} * kWindowMilliseconds / kMillisecondsPerSecond;
    if (config.source != Source::microphone || !window_frames)
        return Result::invalid_argument;
    analyzer.channels = platform::i2s::channel_count(in);
    analyzer.frame_bytes = platform::i2s::frame_bytes(in);
    analyzer.window_samples = window_frames * analyzer.channels;
    analyzer.wide = in.sample_bits != platform::i2s::SampleWidth::bits16;
    clear_statistics(analyzer);
    analyzer.open = true;
    opened = {&analyzer, nullptr, level_write, level_close};
    return Result::success;
}

/** @brief Level in tenths exceeded by `percent` of the windows, at 1 dB resolution. */
std::int32_t exceeded(const Analyzer& a, std::uint32_t percent) {
    const auto target = (std::uint64_t{a.windows} * percent + 99U) / 100U;
    std::uint64_t seen{};
    for (std::size_t i = kBins; i-- > 0;) {
        seen += a.bins[i];
        if (seen >= target && seen)
            return (static_cast<std::int32_t>(i) + kFloorDb) * kTenths + kTenths / 2;
    }
    return kFloorDb * kTenths;
}
} // namespace

void initialize_level() {
    (void)register_endpoint_provider(endpoint);
}

Result level_start() {
    if (status(session).state == State::running)
        return Result::success;
    auto config = command_config();
    config.source = Source::microphone;
    config.sink = Sink::custom;
    config.codec = Codec::none;
    config.loop = false;
    config.location.fill(0);
    std::copy(kEndpointName.begin(), kEndpointName.end(), config.location.begin());
    return start(config, session);
}

Result level_stop() {
    const auto result = stop(session);
    if (result == Result::success)
        session = {};
    return result;
}

void level_reset() {
    clear_statistics(analyzer);
}

void level_set_offset(std::int32_t db) {
    settings.offset_db = db;
}

void level_set_alarm(bool enabled, std::int32_t db) {
    settings.alarm_enabled = enabled;
    settings.alarm_db = db;
    if (!enabled)
        analyzer.alarm_active = false;
}

LevelSnapshot level_snapshot() {
    const auto& a = analyzer;
    LevelSnapshot s{};
    s.running = a.open;
    s.alarm_enabled = settings.alarm_enabled;
    s.alarm_active = a.alarm_active;
    s.windows = a.windows;
    s.clipped = a.clipped;
    s.alarm_count = a.alarm_count;
    s.offset_db = settings.offset_db;
    s.alarm_db = settings.alarm_db;
    if (!a.windows)
        return s;
    const auto shift = settings.offset_db * kTenths;
    s.level = a.level + shift;
    s.peak = a.peak + shift;
    s.min = a.min_level + shift;
    s.max = a.max_level + shift;
    s.leq = tenths(10.0 * std::log10(a.energy / a.windows)) + shift;
    s.l10 = exceeded(a, 10) + shift;
    s.l50 = exceeded(a, 50) + shift;
    s.l90 = exceeded(a, 90) + shift;
    return s;
}

std::int32_t level_histogram(std::array<std::uint32_t, kLevelHistogramBuckets>& counts) {
    counts = {};
    for (std::size_t i = 0; i < kBins; ++i)
        counts[i / kLevelHistogramStepDb] += analyzer.bins[i];
    return kFloorDb + settings.offset_db;
}
} // namespace newmota::audio
