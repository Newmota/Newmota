/*
  audio_codecs.cpp - Incremental carried MP3 and AAC codecs

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_codecs.cpp
 * @brief Preserves Tasmota-carried codec cores and bounds decoded frame handoff.
 */
#include "drivers/audio/audio_codecs.h"
#include "newmota_features.h"
#include <algorithm>
#include <cstring>
#include <new>
#if USE_AUDIO_MP3
#include "libmad/config.h"
#include "libmad/mad.h"
#include "shine/layer3.h"
#endif
#if USE_AUDIO_AAC
#ifndef ARDUINO
#define ARDUINO 1
#define NEWMOTA_UNDEF_ARDUINO
#endif
#include "libhelix-aac/aacdec.h"
#ifdef NEWMOTA_UNDEF_ARDUINO
#undef ARDUINO
#endif
#endif

namespace newmota::audio {
namespace {
constexpr unsigned kMonoChannelCount = platform::i2s::kMonoChannelCount;
constexpr unsigned kStereoChannelCount = platform::i2s::kStereoChannelCount;
constexpr std::size_t kPcm16SampleBytes = sizeof(std::int16_t);
#if USE_AUDIO_MP3 || USE_AUDIO_AAC
void sample(std::uint8_t* target, std::int16_t value) {
    target[0] = static_cast<std::uint8_t>(value);
    target[1] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(value) >> 8U);
}
#endif
#if USE_AUDIO_MP3
constexpr unsigned kMaximumMp3SamplesPerChannelPerStep = 32;
constexpr std::uint32_t kLowRateMp3ThresholdHz = 32000;
constexpr int kLowRateMp3BitrateKbps = 32;
constexpr int kStandardMp3BitrateKbps = 128;

struct Mp3 {
    mad_stream stream{};
    mad_frame frame{};
    mad_synth synth{};
    std::array<std::uint8_t, kBlockBytes + MAD_BUFFER_GUARD> guarded{};
    unsigned ns{}, ns_end{};

    Mp3() {
        mad_stream_init(&stream);
        mad_frame_init(&frame);
        mad_synth_init(&synth);
    }

    ~Mp3() {
        mad_synth_finish(&synth);
        mad_frame_finish(&frame);
        mad_stream_finish(&stream);
    }
};

Step mp3_process(void* context, const std::uint8_t* input, std::size_t size, bool eof,
    std::uint8_t* output, std::size_t capacity) {
    auto& m = *static_cast<Mp3*>(context);
    Step step{};
    if (m.ns == m.ns_end) {
        m.ns = m.ns_end = 0;
        if (size == 0U) {
            step.finished = eof;
            return step;
        }
        const auto* data = input;
        auto buffered = size;
        if (eof) {
            std::copy_n(input, size, m.guarded.data());
            std::fill_n(m.guarded.data() + size, MAD_BUFFER_GUARD, 0);
            data = m.guarded.data();
            buffered += MAD_BUFFER_GUARD;
        }
        mad_stream_buffer(&m.stream, data, static_cast<unsigned long>(buffered));
        if (mad_frame_decode(&m.frame, &m.stream) == -1) {
            if (m.stream.next_frame && m.stream.next_frame >= data &&
                m.stream.next_frame <= data + buffered)
                step.consumed =
                    std::min(size, static_cast<std::size_t>(m.stream.next_frame - data));
            if (m.stream.error == MAD_ERROR_BUFLEN) {
                if (m.stream.this_frame && m.stream.this_frame >= data &&
                    m.stream.this_frame <= data + buffered)
                    step.consumed =
                        std::min(size, static_cast<std::size_t>(m.stream.this_frame - data));
                if (eof) {
                    step.consumed = size;
                    step.finished = true;
                }
            } else if (!MAD_RECOVERABLE(m.stream.error))
                step.result = Result::codec_error;
            else if (!step.consumed)
                step.consumed = 1;
            return step;
        }
        step.consumed = std::min(size, static_cast<std::size_t>(m.stream.next_frame - data));
        m.ns_end = MAD_NSBSAMPLES(&m.frame.header);
    }
    const auto channels = static_cast<unsigned>(MAD_NCHANNELS(&m.frame.header));
    if (channels != kMonoChannelCount && channels != kStereoChannelCount) {
        step.result = Result::unsupported;
        return step;
    }
    while (m.ns < m.ns_end && capacity - step.produced >= kMaximumMp3SamplesPerChannelPerStep *
                                                              channels * kPcm16SampleBytes) {
        if (mad_synth_frame_onens(&m.synth, &m.frame, m.ns++) != MAD_FLOW_CONTINUE) {
            step.result = Result::codec_error;
            return step;
        }
        for (unsigned i = 0; i < m.synth.pcm.length; ++i)
            for (unsigned c = 0; c < channels; ++c)
                sample(output + step.produced + (i * channels + c) * kPcm16SampleBytes,
                    m.synth.pcm.samples[c][i]);
        step.produced += m.synth.pcm.length * channels * kPcm16SampleBytes;
    }
    step.sample_rate = m.frame.header.samplerate;
    step.channels = static_cast<std::uint8_t>(channels);
    return step;
}

Result mp3_reset(void* context) {
    auto* m = static_cast<Mp3*>(context);
    m->~Mp3();
    new (m) Mp3();
    return Result::success;
}

void mp3_close(void* context) {
    delete static_cast<Mp3*>(context);
}

struct Shine {
    shine_t encoder{};
    std::array<std::int16_t, SHINE_MAX_SAMPLES * kStereoChannelCount> pcm{};
    std::size_t filled{}, needed{};
    bool flushed{};

    ~Shine() {
        if (encoder)
            shine_close(encoder);
    }
};

Step shine_process(void* context, const std::uint8_t* input, std::size_t size, bool eof,
    std::uint8_t* output, std::size_t capacity) {
    auto& s = *static_cast<Shine*>(context);
    Step result{};
    if (size % kPcm16SampleBytes != 0U) {
        result.result = Result::codec_error;
        return result;
    }
    const auto amount = std::min(size / kPcm16SampleBytes, s.needed - s.filled);
    for (std::size_t i = 0; i < amount; ++i)
        s.pcm[s.filled + i] =
            static_cast<std::int16_t>(std::uint16_t(input[i * kPcm16SampleBytes]) |
                                      (std::uint16_t(input[i * kPcm16SampleBytes + 1U]) << 8U));
    s.filled += amount;
    result.consumed = amount * kPcm16SampleBytes;
    if (s.filled == s.needed || (eof && s.filled)) {
        std::fill(s.pcm.begin() + s.filled, s.pcm.begin() + s.needed, 0);
        int written{};
        const auto* bytes = shine_encode_buffer_interleaved(s.encoder, s.pcm.data(), &written);
        if (written < 0 || static_cast<std::size_t>(written) > capacity || (!bytes && written)) {
            result.result = Result::codec_error;
            return result;
        }
        std::copy_n(bytes, written, output);
        result.produced = static_cast<std::size_t>(written);
        s.filled = 0;
    } else if (eof && result.consumed == size && !s.flushed) {
        int written{};
        const auto* bytes = shine_flush(s.encoder, &written);
        if (written < 0 || static_cast<std::size_t>(written) > capacity || (!bytes && written)) {
            result.result = Result::codec_error;
            return result;
        }
        std::copy_n(bytes, written, output);
        result.produced = static_cast<std::size_t>(written);
        s.flushed = result.finished = true;
    } else if (s.flushed)
        result.finished = true;
    return result;
}

void shine_close_stage(void* context) {
    delete static_cast<Shine*>(context);
}

Result open_mp3(const Config& config, bool encode, Stage& stage) {
    if (!encode) {
        auto* m = new (std::nothrow) Mp3();
        if (!m)
            return Result::no_memory;
        stage = {m, mp3_process, mp3_reset, mp3_close};
        return Result::success;
    }
    const auto channels = platform::i2s::channel_count(config.input);
    if (channels < kMonoChannelCount || channels > kStereoChannelCount)
        return Result::unsupported;
    shine_config_t options{};
    shine_set_config_mpeg_defaults(&options.mpeg);
    options.wave.channels = channels == kMonoChannelCount ? PCM_MONO : PCM_STEREO;
    options.wave.samplerate = static_cast<int>(config.input.sample_rate);
    options.mpeg.mode = channels == kMonoChannelCount ? MONO : STEREO;
    options.mpeg.bitr = config.input.sample_rate < kLowRateMp3ThresholdHz ? kLowRateMp3BitrateKbps
                                                                          : kStandardMp3BitrateKbps;
    if (shine_check_config(options.wave.samplerate, options.mpeg.bitr) < 0)
        return Result::unsupported;
    auto* s = new (std::nothrow) Shine();
    if (!s)
        return Result::no_memory;
    s->encoder = shine_initialise(&options);
    if (!s->encoder) {
        delete s;
        return Result::no_memory;
    }
    s->needed = static_cast<std::size_t>(shine_samples_per_pass(s->encoder)) * channels;
    if (s->needed > s->pcm.size()) {
        delete s;
        return Result::codec_error;
    }
    stage = {s, shine_process, nullptr, shine_close_stage,
        static_cast<std::uint32_t>(shine_samples_per_pass(s->encoder))};
    return Result::success;
}
#endif
#if USE_AUDIO_AAC
constexpr std::size_t kAacPcmBufferSamples = 4096;
constexpr std::size_t kAacAdtsHeaderBytes = 7;
constexpr std::size_t kAdtsFrameLengthHighByteOffset = 3;
constexpr std::size_t kAdtsFrameLengthMiddleByteOffset = 4;
constexpr std::size_t kAdtsFrameLengthLowByteOffset = 5;
constexpr std::uint8_t kAdtsSyncFirstByte = 0xFFU;
constexpr std::uint8_t kAdtsSyncMask = 0xF6U;
constexpr std::uint8_t kAdtsSyncValue = 0xF0U;

/** @brief Extracts the 13-bit encoded frame length from an ADTS header. */
std::size_t adts_frame_length(const std::uint8_t* header) {
    return (std::size_t(header[kAdtsFrameLengthHighByteOffset] & 0x03U) << 11U) |
           (std::size_t(header[kAdtsFrameLengthMiddleByteOffset]) << 3U) |
           (header[kAdtsFrameLengthLowByteOffset] >> 5U);
}

struct Aac {
    HAACDecoder decoder{};
    std::array<std::int16_t, kAacPcmBufferSamples> pcm{};
    std::size_t pending{}, offset{};
    AACFrameInfo info{};

    ~Aac() {
        if (decoder)
            AACFreeDecoder(decoder);
    }
};

Step aac_process(void* context, const std::uint8_t* input, std::size_t size, bool eof,
    std::uint8_t* output, std::size_t capacity) {
    auto& a = *static_cast<Aac*>(context);
    Step result{};
    if (a.offset == a.pending) {
        a.offset = a.pending = 0;
        if (size == 0) {
            result.finished = eof;
            return result;
        }
        // ADTS carries a bounded length; preserve incomplete frames without modifying decoder
        // state.
        std::size_t sync{};
        while (sync + 1 < size && !(input[sync] == kAdtsSyncFirstByte &&
                                      (input[sync + 1] & kAdtsSyncMask) == kAdtsSyncValue))
            ++sync;
        if (sync) {
            result.consumed = eof && sync + 1 == size ? size : sync;
            result.finished = eof && result.consumed == size;
            return result;
        }
        if (size < kAacAdtsHeaderBytes) {
            if (eof)
                result.result = Result::codec_error;
            return result;
        }
        const auto length = adts_frame_length(input);
        if (length < kAacAdtsHeaderBytes || length > kBlockBytes) {
            result.result = Result::unsupported;
            return result;
        }
        if (size < length) {
            if (eof)
                result.result = Result::codec_error;
            return result;
        }
        auto* data = const_cast<unsigned char*>(input);
        auto remaining = static_cast<int>(length);
        if (AACDecode(a.decoder, &data, &remaining, a.pcm.data()) != 0) {
            result.result = Result::codec_error;
            return result;
        }
        AACGetLastFrameInfo(a.decoder, &a.info);
        if (a.info.nChans < kMonoChannelCount || a.info.nChans > kStereoChannelCount ||
            a.info.outputSamps < 0 || static_cast<std::size_t>(a.info.outputSamps) > a.pcm.size()) {
            result.result = Result::unsupported;
            return result;
        }
        result.consumed = length;
        a.pending = static_cast<std::size_t>(a.info.outputSamps);
    }
    const auto channels = static_cast<std::size_t>(a.info.nChans);
    const auto count =
        std::min(a.pending - a.offset, capacity / (kPcm16SampleBytes * channels) * channels);
    for (std::size_t i = 0; i < count; ++i)
        sample(output + i * kPcm16SampleBytes, a.pcm[a.offset + i]);
    a.offset += count;
    result.produced = count * kPcm16SampleBytes;
    result.sample_rate = static_cast<std::uint32_t>(a.info.sampRateOut);
    result.channels = static_cast<std::uint8_t>(channels);
    return result;
}

Result aac_reset(void* context) {
    auto& a = *static_cast<Aac*>(context);
    a.pending = a.offset = 0;
    return AACFlushCodec(a.decoder) == 0 ? Result::success : Result::codec_error;
}

void aac_close(void* context) {
    delete static_cast<Aac*>(context);
}

Result open_aac(bool encode, Stage& stage) {
    if (encode)
        return Result::unsupported;
    auto* a = new (std::nothrow) Aac();
    if (!a)
        return Result::no_memory;
    a->decoder = AACInitDecoder();
    if (!a->decoder) {
        delete a;
        return Result::no_memory;
    }
    stage = {a, aac_process, aac_reset, aac_close};
    return Result::success;
}
#endif
} // namespace

Result open_codec(const Config& config, bool encode, Stage& stage) {
    stage = {};
    (void)encode;
    switch (config.codec) {
#if USE_AUDIO_MP3
    case Codec::mp3:
        return open_mp3(config, encode, stage);
#endif
#if USE_AUDIO_AAC
    case Codec::aac:
        return open_aac(encode, stage);
#endif
#if USE_AUDIO_OPUS
    case Codec::opus:
        return detail::open_opus(config, encode, stage);
#endif
#if USE_AUDIO_SAM
    case Codec::sam:
        return detail::open_sam(config, encode, stage);
#endif
#if USE_BERRY_AUDIO
    case Codec::tone:
        return detail::open_tone(config, encode, stage);
#endif
    default:
        return Result::unsupported;
    }
}
} // namespace newmota::audio
