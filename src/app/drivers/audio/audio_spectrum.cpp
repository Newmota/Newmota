/*
  audio_spectrum.cpp - Microphone third-octave spectrum analyzer

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_spectrum.cpp
 * @brief Hann-windowed radix-2 FFT with one transform per endpoint write.
 *
 * The FFT is esp-dsp's assembly kernel on the ESP32-S3 and P4 (USE_ESP_DSP), CMSIS-DSP on the
 * RP2350 and AmebaD (USE_CMSIS_DSP) and the portable transform below elsewhere; all work on
 * interleaved complex points.
 *
 * The analyzer keeps the newest `fft_size` mono samples. When at least half a frame of new
 * samples has arrived at the end of a write, it transforms the newest frame once, so a slow
 * target skips frames instead of falling behind. Band levels are mean squares relative to
 * full scale (a full-scale sine reads -3 dB), like the level meter.
 */
#include "drivers/audio/audio_spectrum.h"
#include "newmota_features.h"
#if USE_ESP_DSP || USE_CMSIS_DSP
#include "newmota/platform/fft.h"
#endif
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string_view>

namespace newmota::audio {
namespace {
constexpr std::string_view kEndpointName = "spectrum";
constexpr std::size_t kMinimumFftSize = 512;
constexpr std::size_t kMaximumFftSize = 2048;
/** @brief The FFT size is the smallest one whose bin width is at most this many Hz. */
constexpr std::uint32_t kTargetBinHz = 16;
constexpr int kFloorDb = -120;
constexpr int kTenths = 10;
constexpr std::size_t kPcm16Bytes = 2;
constexpr std::size_t kPcm32Bytes = 4;
constexpr float kScale16 = 1.0F / 32768.0F;
constexpr float kScale32 = 1.0F / 2147483648.0F;
constexpr double kTwoPi = 6.283185307179586;
/** @brief The esp-dsp kernels load 64-bit pairs; 16 bytes also suits the vector unit. */
constexpr std::size_t kBufferAlignment = 16;
/** @brief Spare floats that let the buffers start on kBufferAlignment. */
constexpr std::size_t kAlignmentSlack = kBufferAlignment / sizeof(float);
/** @brief Twiddle floats per FFT point; CMSIS-DSP's tables are constants in flash. */
constexpr std::size_t kTwiddleFloatsPerPoint = USE_CMSIS_DSP ? 0U : 1U;
/** @brief Nominal ISO third-octave centers; the exact ones are 1 kHz * 2^((i - 16) / 3). */
constexpr std::array<std::uint32_t, kSpectrumMaximumBands> kNominalCenters{25, 31, 40, 50, 63, 80,
    100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000,
    5000, 6300, 8000, 10000, 12500, 16000, 20000};
constexpr int kReferenceBand = 16;

struct Band {
    std::uint32_t center{};
    std::size_t first{}, end{};
};

struct Analyzer {
    bool open{}, wide{};
    std::size_t frame_bytes{}, size{}, position{}, filled{}, since{};
    unsigned channels{};
    std::uint32_t rate{};
    /** @brief Ring, interleaved complex frame, window and twiddle table; freed on close. */
    std::unique_ptr<float[]> memory;
    float *ring{}, *data{}, *window{}, *twiddle{};
    float scale{}, sample_scale{};
    std::array<Band, kSpectrumMaximumBands> bands{};
    std::size_t band_count{};
    std::uint32_t frames{};
    std::array<float, kSpectrumMaximumBands> latest{}, hold{};
    std::array<double, kSpectrumMaximumBands> energy{};
    std::uint32_t peak_hz{};
    float peak_energy{};
};

Analyzer analyzer;
Handle session{};

std::int32_t tenths(double mean_square) {
    const auto db = mean_square > 0 ? 10.0 * std::log10(mean_square) : double{kFloorDb};
    return static_cast<std::int32_t>(std::lround(std::clamp(db, double{kFloorDb}, 0.0) * kTenths));
}

void clear_statistics(Analyzer& a) {
    a.frames = 0;
    a.latest = {};
    a.hold = {};
    a.energy = {};
    a.peak_hz = 0;
    a.peak_energy = 0;
}

bool allocate(Analyzer& a) {
    const auto n = a.size;
    // The aligned operator new reaches libstdc++'s throwing allocator, which aborts on ESP-IDF
    // and links the unwinder elsewhere; the plain nothrow form returns null, so align by hand.
    a.memory.reset(new (std::nothrow) float[n * (4U + kTwiddleFloatsPerPoint) + kAlignmentSlack]);
    if (!a.memory)
        return false;
    const auto address = reinterpret_cast<std::uintptr_t>(a.memory.get());
    a.ring = a.memory.get() +
             (kBufferAlignment - address % kBufferAlignment) % kBufferAlignment / sizeof(float);
    a.data = a.ring + n;
    a.window = a.data + 2U * n;
    a.twiddle = a.window + n;
    double window_power{};
    for (std::size_t i = 0; i < n; ++i) {
        const auto w =
            0.5 * (1.0 - std::cos(kTwoPi * static_cast<double>(i) / static_cast<double>(n)));
        a.window[i] = static_cast<float>(w);
        window_power += w * w;
        a.ring[i] = 0;
    }
#if USE_ESP_DSP
    platform::fft::prepare(a.twiddle, n);
#elif !USE_CMSIS_DSP
    for (std::size_t i = 0; i < n / 2U; ++i) {
        const auto angle = kTwoPi * static_cast<double>(i) / static_cast<double>(n);
        a.twiddle[2U * i] = static_cast<float>(std::cos(angle));
        a.twiddle[2U * i + 1U] = static_cast<float>(std::sin(angle));
    }
#endif
    // Parseval for a windowed frame: E[sum |X|^2] = n * sum(w^2) * mean_square.
    a.scale = static_cast<float>(1.0 / (static_cast<double>(n) * window_power));
    a.position = a.filled = a.since = 0;
    return true;
}

/** @brief Maps third-octave bands onto bins; bands without a bin center are left out. */
void plan_bands(Analyzer& a) {
    a.band_count = 0;
    const auto bin_hz = static_cast<double>(a.rate) / static_cast<double>(a.size);
    const auto half = static_cast<std::size_t>(a.size / 2U);
    for (int i = 0; i < static_cast<int>(kSpectrumMaximumBands); ++i) {
        const auto center = 1000.0 * std::exp2((i - kReferenceBand) / 3.0);
        const auto upper = center * std::exp2(1.0 / 6.0);
        // A band that reaches past Nyquist would read low, so it is left out.
        if (upper > static_cast<double>(a.rate) / 2.0)
            break;
        const auto first =
            static_cast<std::size_t>(std::ceil(center * std::exp2(-1.0 / 6.0) / bin_hz));
        const auto end = std::min(static_cast<std::size_t>(std::ceil(upper / bin_hz)), half);
        if (first < 1U || end <= first)
            continue;
        a.bands[a.band_count++] = {kNominalCenters[static_cast<std::size_t>(i)], first, end};
    }
}

/** @brief In-place FFT of the interleaved frame in `data`, in natural order. */
void transform(Analyzer& a) {
    const auto n = a.size;
#if USE_ESP_DSP || USE_CMSIS_DSP
    platform::fft::forward(a.data, a.twiddle, n);
#else
    auto* const d = a.data;
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        auto bit = n >> 1U;
        for (; j & bit; bit >>= 1U)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(d[2U * i], d[2U * j]);
            std::swap(d[2U * i + 1U], d[2U * j + 1U]);
        }
    }
    // Radix-2 decimation in time.
    for (std::size_t length = 2; length <= n; length <<= 1U) {
        const auto half = length / 2U;
        const auto step = n / length;
        for (std::size_t start = 0; start < n; start += length)
            for (std::size_t k = 0; k < half; ++k) {
                const auto wr = a.twiddle[2U * k * step];
                const auto ws = a.twiddle[2U * k * step + 1U];
                auto* const lo = d + 2U * (start + k);
                auto* const hi = lo + 2U * half;
                const auto tr = hi[0] * wr + hi[1] * ws;
                const auto ti = hi[1] * wr - hi[0] * ws;
                hi[0] = lo[0] - tr;
                hi[1] = lo[1] - ti;
                lo[0] += tr;
                lo[1] += ti;
            }
    }
#endif
}

void analyze_frame(Analyzer& a) {
    const auto n = a.size;
    float mean{};
    for (std::size_t i = 0; i < n; ++i)
        mean += a.ring[i];
    mean /= static_cast<float>(n);
    // `position` is the oldest sample, so the frame is rebuilt in time order.
    for (std::size_t i = 0; i < n; ++i) {
        a.data[2U * i] = (a.ring[(a.position + i) & (n - 1U)] - mean) * a.window[i];
        a.data[2U * i + 1U] = 0;
    }
    transform(a);
    const auto half = n / 2U;
    // One-sided power per bin: bins 1..n/2-1 stand for their negative-frequency twin too.
    const auto power = [&](std::size_t k) {
        return a.data[2U * k] * a.data[2U * k] + a.data[2U * k + 1U] * a.data[2U * k + 1U];
    };
    for (std::size_t b = 0; b < a.band_count; ++b) {
        float sum{};
        for (auto k = a.bands[b].first; k < a.bands[b].end; ++k)
            sum += power(k);
        const auto mean_square = 2.0F * sum * a.scale;
        a.latest[b] = mean_square;
        a.hold[b] = std::max(a.hold[b], mean_square);
        a.energy[b] += mean_square;
    }
    std::size_t peak = 1;
    for (std::size_t k = 2; k < half; ++k)
        if (power(k) > power(peak))
            peak = k;
    const auto before = power(peak - 1U);
    const auto after = peak + 1U < half ? power(peak + 1U) : 0.0F;
    a.peak_energy = 2.0F * (before + power(peak) + after) * a.scale;
    // Parabolic interpolation in the log domain refines the frequency below one bin.
    double offset{};
    if (peak >= 2U && peak + 1U < half && before > 0 && after > 0) {
        const auto x = std::log(static_cast<double>(before));
        const auto y = std::log(static_cast<double>(power(peak)));
        const auto z = std::log(static_cast<double>(after));
        const auto denominator = x - 2.0 * y + z;
        if (denominator < 0)
            offset = std::clamp(0.5 * (x - z) / denominator, -0.5, 0.5);
    }
    // Silence has no dominant tone.
    a.peak_hz = a.peak_energy > 0
                    ? static_cast<std::uint32_t>(std::lround(
                          (static_cast<double>(peak) + offset) * a.rate / static_cast<double>(n)))
                    : 0U;
    ++a.frames;
}

template <typename Load>
void push(
    Analyzer& a, const std::uint8_t* bytes, std::size_t frames, std::size_t stride, Load load) {
    const auto mask = a.size - 1U;
    for (std::size_t f = 0; f < frames; ++f, bytes += a.frame_bytes) {
        float sum{};
        for (unsigned c = 0; c < a.channels; ++c)
            sum += load(bytes + c * stride);
        a.ring[a.position] = sum * a.sample_scale;
        a.position = (a.position + 1U) & mask;
    }
    a.filled = std::min(a.filled + frames, a.size);
    a.since += frames;
}

Io spectrum_write(void* context, const std::uint8_t* bytes, std::size_t size) {
    auto& a = *static_cast<Analyzer*>(context);
    const auto frames = size / a.frame_bytes;
    if (a.wide)
        push(a, bytes, frames, kPcm32Bytes, [](const std::uint8_t* p) {
            std::int32_t value;
            std::memcpy(&value, p, sizeof(value));
            return static_cast<float>(value);
        });
    else
        push(a, bytes, frames, kPcm16Bytes, [](const std::uint8_t* p) {
            std::int16_t value;
            std::memcpy(&value, p, sizeof(value));
            return static_cast<float>(value);
        });
    if (a.filled == a.size && a.since >= a.size / 2U) {
        analyze_frame(a);
        a.since = 0;
    }
    return {Result::success, frames * a.frame_bytes};
}

void spectrum_close(void* context) {
    auto& a = *static_cast<Analyzer*>(context);
    a.open = false;
    a.memory.reset();
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
    if (config.source != Source::microphone || !in.sample_rate)
        return Result::invalid_argument;
    auto& a = analyzer;
    a.rate = in.sample_rate;
    a.size = kMinimumFftSize;
    while (a.size < kMaximumFftSize && a.rate / a.size > kTargetBinHz)
        a.size *= 2U;
    a.channels = platform::i2s::channel_count(in);
    a.frame_bytes = platform::i2s::frame_bytes(in);
    a.wide = in.sample_bits != platform::i2s::SampleWidth::bits16;
    // The channels are averaged: fold the 1/channels and the integer scale into one factor.
    a.sample_scale = (a.wide ? kScale32 : kScale16) / static_cast<float>(a.channels);
    plan_bands(a);
    if (!a.band_count)
        return Result::invalid_argument;
    if (!allocate(a))
        return Result::no_memory;
    clear_statistics(a);
    a.open = true;
    opened = {&a, nullptr, spectrum_write, spectrum_close};
    return Result::success;
}
} // namespace

void initialize_spectrum() {
    (void)register_endpoint_provider(endpoint);
}

Result spectrum_start() {
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

Result spectrum_stop() {
    const auto result = stop(session);
    if (result == Result::success)
        session = {};
    return result;
}

void spectrum_reset() {
    clear_statistics(analyzer);
}

SpectrumSnapshot spectrum_snapshot() {
    const auto& a = analyzer;
    SpectrumSnapshot s{};
    s.running = a.open;
    s.frames = a.frames;
    s.fft_size = static_cast<std::uint32_t>(a.size);
    s.bands = static_cast<std::uint32_t>(a.band_count);
    s.peak_hz = a.peak_hz;
    s.peak_level = tenths(a.peak_energy);
    return s;
}

std::size_t spectrum_series(SpectrumSeries series,
    std::array<std::uint32_t, kSpectrumMaximumBands>& centers,
    std::array<std::int32_t, kSpectrumMaximumBands>& levels) {
    const auto& a = analyzer;
    if (!a.frames)
        return 0;
    for (std::size_t b = 0; b < a.band_count; ++b) {
        centers[b] = a.bands[b].center;
        levels[b] = tenths(series == SpectrumSeries::latest ? double{a.latest[b]}
                           : series == SpectrumSeries::hold ? double{a.hold[b]}
                                                            : a.energy[b] / a.frames);
    }
    return a.band_count;
}
} // namespace newmota::audio
