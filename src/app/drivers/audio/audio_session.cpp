/*
  audio_session.cpp - Bounded native audio sessions

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_session.cpp
 * @brief Advances each audio session by bounded native block operations.
 */
#include "drivers/audio/audio_session.h"
#include "core/storage/filesystem.h"
#include "drivers/audio/audio_ring.h"
#include "drivers/audio/audio_settings.h"
#include "newmota/platform/platform.h"
#include "newmota/platform/audio_worker.h"
#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <new>
#include <cstring>

namespace newmota::audio {
namespace {
constexpr std::uint32_t kTransmitDrainTimeoutMilliseconds = 2000;
/** @brief Bounds codec/copy work before the worker yields to its host. */
constexpr unsigned kMaximumCaptureStepsPerWake = 16;
/** @brief Bounds decode-ahead work per application poll. */
constexpr unsigned kMaximumDecodeStepsPerPoll = 8;
/** @brief Bounds sink writes per application poll. */
constexpr unsigned kMaximumSinkWritesPerPoll = 8;
/** @brief Bounds PCM hand-offs to the I2S driver per feed wake. */
constexpr unsigned kMaximumFeedWritesPerWake = 4;
/** @brief Native capture task floor shared by raw and MP3 capture. */
constexpr std::size_t kCaptureTaskStackBytes = 8192;
/** @brief Fixed portion of Tasmota's proven Opus capture stack reservation. */
constexpr std::size_t kOpusCaptureStackBaseBytes = 30000;
/** @brief Caps polling latency; codec passes are at most about 36 ms. */
constexpr std::uint32_t kMaximumCapturePeriodMilliseconds = 40;
/** @brief The capture period never exceeds this many DMA pages (half the usual ring). */
constexpr std::uint32_t kCapturePeriodDmaPages = 2;
/** @brief Feed task period cap; the PCM ring keeps data queued between wakes. */
constexpr std::uint32_t kFeedMaximumPeriodMilliseconds = 20;
/** @brief Encoded bytes (or raw PCM) queued between the capture task and the sink owner.
 * One second of 128 kbit/s MP3, about 0.5 s of 16 kHz mono raw PCM.
 */
constexpr std::size_t kCaptureRingBytes = 16384;
/** @brief Decoded PCM queued between the application decoder and the feed task. */
constexpr std::size_t kPlaybackRingBytes = 16384;
/** @brief Playback starts once this much PCM is queued, or the stream has ended. */
constexpr std::size_t kPlaybackPrefillBytes = 8192;
/** @brief Smallest output span offered to a decoder; fits one MP3 granule or AAC frame. */
constexpr std::size_t kMinimumDecodeSpaceBytes = 512;
/** @brief Compressed input is topped up only below this level (or when the codec starves). */
constexpr std::size_t kRefillLowWaterBytes = 2048;

/** @brief One producer (capture task), one consumer (application sink owner).
 * A full ring terminates capture explicitly; encoded bytes are never silently lost.
 */
struct Capture {
    platform::audio_worker::Worker worker{};
    Ring ring{};
    std::atomic<std::uint32_t> input_bytes{0};
    std::atomic<Result> result{Result::success};
    std::atomic<bool> finishing{false}, finished{false};
};

/** @brief The application decodes ahead into `ring`; the feed task alone drives I2S TX.
 * A stall of the application loop therefore only underruns after the ring drains.
 */
struct Playback {
    platform::audio_worker::Worker worker{};
    Ring ring{};
    std::atomic<Result> result{Result::success};
    std::atomic<bool> producer_done{false}, drained{false}, paused{false};
    std::atomic<std::uint32_t> output_bytes{0};
    /** Feed task only. */
    std::uint32_t drain_started{};
    bool started{}, draining{};
};

struct Session {
    Config config{};
    Status status{};
    std::uint32_t generation{}, started_at{}, drain_started{};
    i2s::Handle input{}, output{}, lease{};
    filesystem::FileReader reader{};
    filesystem::FileWriter writer{};
    Endpoint source{}, sink{};
    Stage codec{};
    std::unique_ptr<Capture> capture{};
    std::unique_ptr<Playback> playback{};
    std::array<std::uint8_t, kBlockBytes> incoming{}, outgoing{};
    /** Unconsumed compressed input is incoming[input_offset, input_size). */
    std::size_t input_size{}, input_offset{}, output_size{}, output_offset{};
    bool eof{}, finished{}, paired{}, has_data{}, draining{}, starved{}, paused{};
};

std::array<Session, kSessionCount> sessions{};
std::uint32_t next_generation{};
CodecFactory codec_factory{};
std::array<EndpointFactory, kMaximumEndpointProviders> endpoint_providers{};
std::size_t endpoint_provider_count{};
StreamControl stream_callback{};
bool streaming{};
Config defaults{};
Handle command{};
std::atomic<std::uint8_t> gain_percent{100};
std::atomic<std::uint16_t> input_gain_q12_4{kUnityInputGain};

Session* resolve(Handle h) {
    if (h.slot >= sessions.size() || !h.generation)
        return nullptr;
    auto& s = sessions[h.slot];
    return s.generation == h.generation ? &s : nullptr;
}

Result hardware(i2s::Result r) {
    if (r == i2s::Result::success)
        return Result::success;
    if (r == i2s::Result::busy)
        return Result::busy;
    if (r == i2s::Result::unsupported)
        return Result::unsupported;
    if (r == i2s::Result::invalid_argument)
        return Result::invalid_argument;
    if (r == i2s::Result::no_memory)
        return Result::no_memory;
    return Result::hardware_error;
}

bool same_clock(const i2s::Config& a, const i2s::Config& b) {
    return (a.controller < 0 || b.controller < 0 || a.controller == b.controller) &&
           (!a.mclk.valid() || !b.mclk.valid() || a.mclk.value() == b.mclk.value()) &&
           a.bclk.value() == b.bclk.value() && a.ws.value() == b.ws.value() &&
           a.sample_rate == b.sample_rate && a.mode == b.mode && a.framing == b.framing &&
           a.role == b.role && a.sample_bits == b.sample_bits && a.slot_bits == b.slot_bits &&
           a.slots == b.slots && a.slot_mask == b.slot_mask && a.msb_right == b.msb_right &&
           a.tuning == b.tuning; // the paired lease opens both channels with one profile
}

/** @brief Joins the playback feed task; no callback runs after this returns. */
void stop_playback(Session& s) {
    if (!s.playback)
        return;
    s.status.worker_stack_free_bytes = platform::audio_worker::stack_free_bytes(s.playback->worker);
    s.status.output_bytes = s.playback->output_bytes.load(std::memory_order_relaxed);
    platform::audio_worker::stop(s.playback->worker);
    s.playback.reset();
}

Result release(Session& s, bool retain_lease = false) {
    // Joining is mandatory before freeing buffers, the codec, or the I2S lease.
    if (s.capture) {
        s.status.worker_stack_free_bytes =
            platform::audio_worker::stack_free_bytes(s.capture->worker);
        platform::audio_worker::stop(s.capture->worker);
        s.capture.reset();
    }
    stop_playback(s);
    bool good = true;
    if (s.lease.generation) {
        const auto result = retain_lease ? i2s::deactivate(s.lease) : i2s::close(s.lease);
        if (result == i2s::Result::success) {
            s.input = s.output = {};
            if (!retain_lease)
                s.lease = {};
        } else
            good = false;
    }
    if (s.reader.native)
        good = filesystem::close(s.reader) && good;
    if (s.writer.native)
        good = filesystem::close(s.writer) && good;
    if (s.source.close)
        s.source.close(s.source.context);
    if (s.sink.close)
        s.sink.close(s.sink.context);
    if (s.codec.close)
        s.codec.close(s.codec.context);
    s.source = {};
    s.sink = {};
    s.codec = {};
    return good ? Result::success : Result::hardware_error;
}

void fail(Session& s, Result reason) {
    s.status.state = State::failed;
    s.status.result = reason;
    (void)release(s);
}

Result open_output(Session& s, std::uint32_t rate, std::uint8_t channels) {
    auto config = s.config.output;
    config.din = {};
    if (rate) {
        if (channels != platform::i2s::kMonoChannelCount &&
            channels != platform::i2s::kStereoChannelCount)
            return Result::codec_error;
        config.sample_rate = rate;
        config.sample_bits = platform::i2s::SampleWidth::bits16;
        config.slots = platform::i2s::kStandardSlotCount;
        config.slot_mask = channels == platform::i2s::kMonoChannelCount
                               ? platform::i2s::kLeftSlotMask
                               : platform::i2s::kStereoSlotMask;
    }
    const auto result = hardware(i2s::activate(s.lease, config));
    if (result == Result::success) {
        s.config.output = config;
        s.output = s.lease;
    }
    return result;
}

Result open_endpoint(const Config& config, bool input, Endpoint& endpoint) {
    for (std::size_t i = 0; i < endpoint_provider_count; ++i) {
        endpoint = {};
        const auto result = endpoint_providers[i](config, input, endpoint);
        if (result != Result::unsupported)
            return result;
    }
    return Result::unsupported;
}

Io read(Session& s, std::uint8_t* bytes, std::size_t capacity) {
    if (s.config.source == Source::microphone) {
        auto r = i2s::read(s.input, bytes, capacity);
        return {hardware(r.result), r.count, false};
    }
    // The tone generator runs until finish() sets eof; SAM has rendered everything already.
    if (s.config.source == Source::text)
        return {Result::success, 0, s.config.codec != Codec::tone};
    if (s.config.source == Source::file) {
        std::size_t size{};
        if (filesystem::read(s.reader, bytes, capacity, size) != filesystem::ReadResult::succeeded)
            return {Result::io_error};
        return {Result::success, size, size == 0U};
    }
    return s.source.read(s.source.context, bytes, capacity);
}

Io write(Session& s, const std::uint8_t* bytes, std::size_t size) {
    if (s.config.sink == Sink::output) {
        auto r = i2s::write(s.output, bytes, size);
        return {hardware(r.result), r.count};
    }
    if (s.config.sink == Sink::file) {
        if (filesystem::write(s.writer, bytes, size) != filesystem::MutationResult::succeeded)
            return {Result::io_error};
        return {Result::success, size};
    }
    return s.sink.write(s.sink.context, bytes, size);
}

/** @brief Scales 16-bit microphone samples by a Q12.4 gain, clipping at full scale. */
void scale_input(std::uint8_t* bytes, std::size_t size, std::uint16_t gain) {
    for (std::size_t i = 0; i + 1 < size; i += sizeof(std::int16_t)) {
        std::int16_t sample{};
        std::memcpy(&sample, bytes + i, sizeof(sample));
        const auto scaled = std::clamp<std::int32_t>(std::int32_t{sample} * gain / kUnityInputGain,
            std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max());
        sample = static_cast<std::int16_t>(scaled);
        std::memcpy(bytes + i, &sample, sizeof(sample));
    }
}

/** @brief Wake period for a capture worker: one codec pass, never beyond a DMA depth. */
std::uint32_t capture_period_milliseconds(const Config& config, const Stage& codec) {
    const auto rate = config.input.sample_rate;
    const auto dma_ms =
        std::uint64_t{config.input.dma_frames} * 1000U * kCapturePeriodDmaPages / rate;
    const auto pass_ms = codec.pass_frames ? std::uint64_t{codec.pass_frames} * 1000U / rate
                                           : std::uint64_t{kMaximumCapturePeriodMilliseconds};
    return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
        std::min(dma_ms, pass_ms), 1U, kMaximumCapturePeriodMilliseconds));
}

/** @brief Reads and encodes whole interleaved frames without calling any sink provider.
 * Only this worker accesses incoming/outgoing staging and the microphone codec. Encoded
 * bytes (or raw PCM without a codec) go straight into the ring; no block staging copy.
 * The bounded work limit allows both RTOS yielding and Pico core-0 cooperation.
 */
void capture_step(void* context) {
    auto& s = *static_cast<Session*>(context);
    auto& capture = *s.capture;
    if (capture.result.load(std::memory_order_relaxed) != Result::success ||
        capture.finished.load(std::memory_order_relaxed))
        return;
    for (unsigned pass = 0; pass < kMaximumCaptureStepsPerWake; ++pass) {
        const bool eof = capture.finishing.load(std::memory_order_acquire);
        std::size_t received{};
        if (!eof && s.input_size < s.incoming.size()) {
            const auto frame = platform::i2s::frame_bytes(s.config.input);
            const auto capacity = (s.incoming.size() - s.input_size) / frame * frame;
            const auto input = i2s::read(s.input, s.incoming.data() + s.input_size, capacity);
            if (input.result != i2s::Result::success || input.count > capacity) {
                capture.result.store(input.result == i2s::Result::success ? Result::hardware_error
                                                                          : hardware(input.result),
                    std::memory_order_release);
                return;
            }
            received = input.count;
            // Encoders take 16-bit PCM only, so the gain never meets wider or raw PDM samples.
            const auto gain = input_gain_q12_4.load(std::memory_order_relaxed);
            if (s.codec.process && gain != kUnityInputGain)
                scale_input(s.incoming.data() + s.input_size, received, gain);
            s.input_size += received;
            capture.input_bytes.fetch_add(received, std::memory_order_relaxed);
        }
        Step step{};
        const std::uint8_t* produced = s.outgoing.data();
        if (s.codec.process)
            step = s.codec.process(s.codec.context, s.incoming.data(), s.input_size, eof,
                s.outgoing.data(), s.outgoing.size());
        else {
            produced = s.incoming.data();
            step.consumed = step.produced = s.input_size;
            step.finished = eof;
        }
        if (step.result != Result::success || step.consumed > s.input_size ||
            step.produced > s.outgoing.size()) {
            capture.result.store(step.result == Result::success ? Result::codec_error : step.result,
                std::memory_order_release);
            return;
        }
        if (step.produced) {
            if (capture.ring.free() < step.produced) {
                capture.result.store(Result::overrun, std::memory_order_release);
                return;
            }
            capture.ring.write(produced, step.produced);
        }
        // The raw route aliases `incoming`, so shift it only after the ring copy.
        s.input_size -= step.consumed;
        if (s.input_size)
            std::memmove(s.incoming.data(), s.incoming.data() + step.consumed, s.input_size);
        if (step.finished) {
            capture.finished.store(true, std::memory_order_release);
            return;
        }
        if (!received && !step.consumed && !step.produced) {
            if (eof || s.input_size == s.incoming.size())
                capture.result.store(Result::codec_error, std::memory_order_release);
            return;
        }
    }
}

/** @brief Releases consumed compressed input; an empty buffer rewinds without copying. */
void consume_input(Session& s, std::size_t count) {
    s.input_offset += count;
    if (s.input_offset == s.input_size)
        s.input_offset = s.input_size = 0;
}

/** @brief Tops up compressed input once it runs low or the codec reports starvation.
 * Unconsumed bytes move to the front only here, so the usual step copies nothing.
 * @return The failure to report, or Success.
 */
Result refill_input(Session& s) {
    const auto remaining = s.input_size - s.input_offset;
    if (s.eof || (!s.starved && remaining >= kRefillLowWaterBytes))
        return Result::success;
    if (s.input_offset) {
        std::memmove(s.incoming.data(), s.incoming.data() + s.input_offset, remaining);
        s.input_offset = 0;
        s.input_size = remaining;
    }
    s.starved = false;
    const auto capacity = s.incoming.size() - s.input_size;
    if (!capacity)
        return Result::success;
    const auto r = read(s, s.incoming.data() + s.input_size, capacity);
    if (r.result != Result::success || r.count > capacity)
        return r.result == Result::success ? Result::io_error : r.result;
    s.input_size += r.count;
    s.status.input_bytes += static_cast<std::uint32_t>(r.count);
    s.has_data = s.has_data || r.count != 0U;
    s.eof = r.eof;
    return Result::success;
}

/** @brief Records that a codec step stalled; fails only when more input cannot help. */
Result note_stalled(Session& s, const Step& step) {
    if (step.consumed || step.produced || step.finished)
        return Result::success;
    if (s.eof || (s.input_offset == 0 && s.input_size == s.incoming.size()))
        return Result::codec_error;
    s.starved = true;
    return Result::success;
}

/** @brief Feed task: moves queued PCM to the I2S driver and detects the TX drain.
 * Runs on the RTOS task (or cooperatively on Pico core 0). It never touches the session
 * state owned by the application loop, and never joins itself.
 */
void feed_step(void* context) {
    auto& s = *static_cast<Session*>(context);
    auto& p = *s.playback;
    if (p.result.load(std::memory_order_relaxed) != Result::success ||
        p.drained.load(std::memory_order_relaxed))
        return;
    const bool done = p.producer_done.load(std::memory_order_acquire);
    if (!p.started) {
        if (p.ring.size() < kPlaybackPrefillBytes && !done)
            return;
        p.started = true;
    }
    if (!p.paused.load(std::memory_order_relaxed)) {
        const auto frame = platform::i2s::frame_bytes(s.config.output);
        for (unsigned pass = 0; pass < kMaximumFeedWritesPerWake; ++pass) {
            const auto span = p.ring.peek(kBlockBytes);
            const auto amount = span.size / frame * frame;
            if (!amount)
                break;
            const auto written = i2s::write(s.output, span.data, amount);
            if (written.result != i2s::Result::success || written.count > amount) {
                p.result.store(written.result == i2s::Result::success ? Result::io_error
                                                                      : hardware(written.result),
                    std::memory_order_release);
                return;
            }
            p.ring.consume(written.count);
            p.output_bytes.fetch_add(
                static_cast<std::uint32_t>(written.count), std::memory_order_relaxed);
            if (written.count < amount)
                break;
        }
    }
    if (!done || !p.ring.empty())
        return;
    // A completed stream must not report Ended until accepted native TX has drained.
    if (i2s::tx_idle(s.output)) {
        p.drained.store(true, std::memory_order_release);
        return;
    }
    const auto now = platform::monotonic_milliseconds();
    if (!p.draining) {
        p.draining = true;
        p.drain_started = now;
    } else if (now - p.drain_started >= kTransmitDrainTimeoutMilliseconds)
        p.result.store(Result::hardware_error, std::memory_order_release);
}

/** @brief Opens TX at the decoded format and starts the feed task. */
Result start_playback(Session& s, std::uint32_t rate, std::uint8_t channels) {
    auto result = open_output(s, rate, channels);
    if (result != Result::success)
        return result;
    s.playback.reset(new (std::nothrow) Playback());
    if (!s.playback || !s.playback->ring.allocate(kPlaybackRingBytes))
        return Result::no_memory;
    s.playback->paused.store(s.paused, std::memory_order_relaxed);
    const auto page_ms = std::uint64_t{s.config.output.dma_frames} * 1000U / rate;
    const auto period_ms = static_cast<std::uint32_t>(
        std::clamp<std::uint64_t>(page_ms, 1U, kFeedMaximumPeriodMilliseconds));
    if (!platform::audio_worker::start(
            s.playback->worker, feed_step, &s, kCaptureTaskStackBytes, period_ms))
        return Result::no_memory;
    return Result::success;
}

/** @brief Applies the software gain to little-endian signed 16-bit PCM in place. */
void apply_gain(std::uint8_t* pcm, std::size_t size) {
    const auto percent = gain_percent.load(std::memory_order_relaxed);
    if (percent >= 100U)
        return;
    for (std::size_t i = 0; i + 1U < size; i += 2U) {
        const auto sample = static_cast<std::int16_t>(
            static_cast<std::uint16_t>(pcm[i] | (std::uint16_t{pcm[i + 1U]} << 8U)));
        const auto scaled = static_cast<std::int16_t>(sample * percent / 100);
        pcm[i] = static_cast<std::uint8_t>(scaled);
        pcm[i + 1U] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(scaled) >> 8U);
    }
}

/** @brief Decodes ahead of the feed task on the serialized application owner. */
void advance_playback(Session& s) {
    s.status.elapsed_seconds = (platform::monotonic_milliseconds() - s.started_at) / 1000U;
    if (s.playback) {
        platform::audio_worker::poll(s.playback->worker);
        s.status.output_bytes = s.playback->output_bytes.load(std::memory_order_relaxed);
        const auto result = s.playback->result.load(std::memory_order_acquire);
        if (result != Result::success) {
            fail(s, result);
            return;
        }
        if (s.playback->drained.load(std::memory_order_acquire)) {
            stop_playback(s);
            s.status.state = State::ended;
            return;
        }
    }
    if (s.paused)
        return;
    for (unsigned pass = 0; pass < kMaximumDecodeStepsPerPoll; ++pass) {
        if (s.finished) {
            if (s.config.loop && s.has_data) {
                if (filesystem::seek(s.reader, 0) != filesystem::ReadResult::succeeded ||
                    !s.codec.reset || s.codec.reset(s.codec.context) != Result::success) {
                    fail(s, Result::io_error);
                    return;
                }
                s.finished = s.eof = s.has_data = s.starved = false;
                s.input_offset = s.input_size = 0;
                ++s.status.loops;
                continue;
            }
            if (!s.playback)
                s.status.state = State::ended; // Nothing was ever decoded or queued.
            else
                s.playback->producer_done.store(true, std::memory_order_release);
            return;
        }
        const auto space = s.playback ? s.playback->ring.free() : kPlaybackRingBytes;
        const auto capacity = std::min(s.outgoing.size(), space) & ~std::size_t{7};
        if (capacity < kMinimumDecodeSpaceBytes)
            return;
        const auto refilled = refill_input(s);
        if (refilled != Result::success) {
            fail(s, refilled);
            return;
        }
        const auto step = s.codec.process(s.codec.context, s.incoming.data() + s.input_offset,
            s.input_size - s.input_offset, s.eof, s.outgoing.data(), capacity);
        if (step.result != Result::success || step.consumed > s.input_size - s.input_offset ||
            step.produced > capacity) {
            fail(s, step.result == Result::success ? Result::codec_error : step.result);
            return;
        }
        if (step.produced) {
            if (!step.sample_rate || !step.channels) {
                fail(s, Result::codec_error);
                return;
            }
            if (!s.playback) {
                const auto opened = start_playback(s, step.sample_rate, step.channels);
                if (opened != Result::success) {
                    fail(s, opened);
                    return;
                }
            } else if (s.config.output.sample_rate != step.sample_rate ||
                       platform::i2s::channel_count(s.config.output) != step.channels) {
                // Preserve queued samples: a midstream format change needs a new session.
                fail(s, Result::unsupported);
                return;
            }
            if (step.produced % platform::i2s::frame_bytes(s.config.output) != 0U) {
                fail(s, Result::codec_error);
                return;
            }
            apply_gain(s.outgoing.data(), step.produced);
            s.playback->ring.write(s.outgoing.data(), step.produced);
        }
        consume_input(s, step.consumed);
        s.finished = step.finished;
        const auto stalled = note_stalled(s, step);
        if (stalled != Result::success) {
            fail(s, stalled);
            return;
        }
        if (!step.consumed && !step.produced && !step.finished)
            return; // Wait for more input; the next poll refills first.
    }
}

void advance(Session& s) {
    s.status.elapsed_seconds = (platform::monotonic_milliseconds() - s.started_at) / 1000U;
    if (s.output_offset < s.output_size) {
        const auto amount = s.output_size - s.output_offset;
        const auto transferred = write(s, s.outgoing.data() + s.output_offset, amount);
        if (transferred.result != Result::success || transferred.count > amount) {
            fail(s, transferred.result == Result::success ? Result::io_error : transferred.result);
            return;
        }
        s.output_offset += transferred.count;
        s.status.output_bytes += static_cast<std::uint32_t>(transferred.count);
        return;
    }
    s.output_offset = s.output_size = 0;
    if (s.finished) {
        if (s.config.loop && s.has_data) {
            if (filesystem::seek(s.reader, 0) != filesystem::ReadResult::succeeded ||
                !s.codec.reset || s.codec.reset(s.codec.context) != Result::success) {
                fail(s, Result::io_error);
                return;
            }
            s.finished = s.eof = s.has_data = false;
            ++s.status.loops;
        } else {
            // A completed route must not report Ended until accepted native TX has drained.
            if (s.output.generation && !i2s::tx_idle(s.output)) {
                const auto now = platform::monotonic_milliseconds();
                if (!s.draining) {
                    s.draining = true;
                    s.drain_started = now;
                } else if (now - s.drain_started >= kTransmitDrainTimeoutMilliseconds) {
                    fail(s, Result::hardware_error);
                }
                return;
            }
            // Retain the endpoint lease until the owner explicitly closes the session.
            if (s.writer.native && !filesystem::close(s.writer)) {
                fail(s, Result::io_error);
                return;
            }
            s.status.state = State::ended;
            return;
        }
    }
    const auto refilled = refill_input(s);
    if (refilled != Result::success) {
        fail(s, refilled);
        return;
    }
    const auto* input = s.incoming.data() + s.input_offset;
    const auto available = s.input_size - s.input_offset;
    if (s.codec.process) {
        const auto step = s.codec.process(
            s.codec.context, input, available, s.eof, s.outgoing.data(), s.outgoing.size());
        if (step.result != Result::success || step.consumed > available ||
            step.produced > s.outgoing.size()) {
            fail(s, step.result == Result::success ? Result::codec_error : step.result);
            return;
        }
        consume_input(s, step.consumed);
        s.output_size = step.produced;
        s.finished = step.finished;
        const auto stalled = note_stalled(s, step);
        if (stalled != Result::success)
            fail(s, stalled);
    } else {
        std::copy_n(input, available, s.outgoing.data());
        s.output_size = available;
        s.input_offset = s.input_size = 0;
        s.finished = s.eof;
    }
}

/** @brief Drains compressed/raw bytes on the serialized application owner. */
void advance_capture(Session& s) {
    auto& capture = *s.capture;
    platform::audio_worker::poll(capture.worker);
    s.status.input_bytes = capture.input_bytes.load(std::memory_order_relaxed);
    s.status.elapsed_seconds = (platform::monotonic_milliseconds() - s.started_at) / 1000U;
    if (capture.worker.native)
        s.status.worker_stack_free_bytes = platform::audio_worker::stack_free_bytes(capture.worker);
    const auto result = capture.result.load(std::memory_order_acquire);
    if (result != Result::success) {
        fail(s, result);
        return;
    }
    const auto frame = s.config.sink == Sink::output ? platform::i2s::frame_bytes(s.config.output)
                                                     : std::size_t{1};
    for (unsigned pass = 0; pass < kMaximumSinkWritesPerPoll; ++pass) {
        const auto span = capture.ring.peek(kBlockBytes);
        const auto amount = span.size / frame * frame;
        if (!amount)
            break;
        const auto transferred = write(s, span.data, amount);
        if (transferred.result != Result::success || transferred.count > amount) {
            fail(s, transferred.result == Result::success ? Result::io_error : transferred.result);
            return;
        }
        s.status.output_bytes += transferred.count;
        capture.ring.consume(transferred.count);
        if (transferred.count < amount)
            break;
    }
    if (capture.finished.load(std::memory_order_acquire) && capture.ring.empty()) {
        platform::audio_worker::stop(capture.worker);
        s.finished = true;
        advance(s);
    }
}
} // namespace

void set_providers(CodecFactory codecs, EndpointFactory endpoints) {
    codec_factory = codecs;
    endpoint_providers = {};
    endpoint_provider_count = 0;
    if (endpoints)
        (void)register_endpoint_provider(endpoints);
}

bool register_endpoint_provider(EndpointFactory provider) {
    if (!provider)
        return false;
    for (std::size_t i = 0; i < endpoint_provider_count; ++i)
        if (endpoint_providers[i] == provider)
            return true;
    if (endpoint_provider_count == endpoint_providers.size())
        return false;
    endpoint_providers[endpoint_provider_count++] = provider;
    return true;
}

Result start(const Config& config, Handle& handle) {
    const bool encoding =
        config.codec != Codec::none &&
        (config.source == Source::microphone || config.source == Source::custom) &&
        (config.sink == Sink::file || config.sink == Sink::network ||
            config.sink == Sink::custom) &&
        !(config.source == Source::custom && config.sink == Sink::custom);
    const bool decoding = config.codec != Codec::none &&
                          (config.source == Source::file || config.source == Source::network ||
                              config.source == Source::custom || config.source == Source::text) &&
                          config.sink == Sink::output;
    const bool raw_live =
        config.codec == Codec::none &&
        (config.source == Source::microphone || config.source == Source::custom) &&
        (config.sink == Sink::output || config.sink == Sink::custom);
    if ((config.loop && config.source != Source::file) ||
        (config.source == Source::text) !=
            (config.codec == Codec::sam || config.codec == Codec::tone) ||
        config.location.back() != '\0' || (config.codec == Codec::none && !raw_live) ||
        (config.codec != Codec::none && !encoding && !decoding) ||
        (config.source == Source::file && config.sink == Sink::file) ||
        (config.source == Source::custom && config.sink == Sink::custom &&
            config.codec != Codec::none))
        return Result::invalid_argument;
    if (raw_live && config.sink == Sink::output &&
        (config.input.sample_rate != config.output.sample_rate ||
            config.input.sample_bits != config.output.sample_bits ||
            platform::i2s::channel_count(config.input) !=
                platform::i2s::channel_count(config.output) ||
            (config.input.mode == platform::i2s::Mode::pdm && !config.input.pdm_pcm)))
        return Result::invalid_argument;
    // Incompatible TX/RX profiles share this owner's controller sequentially.
    // Only the compatible paired standard/TDM route is simultaneous.
    if (config.source == Source::microphone && config.sink == Sink::output &&
        !same_clock(config.input, config.output))
        return Result::unsupported;
    if (encoding &&
        (config.input.sample_bits != platform::i2s::SampleWidth::bits16 ||
            platform::i2s::channel_count(config.input) > platform::i2s::kStereoChannelCount ||
            (config.input.mode == platform::i2s::Mode::pdm && !config.input.pdm_pcm)))
        return Result::unsupported;
    Session* selected = resolve(handle);
    std::size_t slot = selected ? handle.slot : 0;
    i2s::Handle retained{};
    if (selected) {
        if (selected->status.state == State::running)
            return Result::busy;
        if (selected->lease.generation) {
            const auto checked =
                hardware(i2s::check_profiles(selected->lease, config.input, config.output));
            if (checked != Result::success)
                return checked;
        }
        // A stopped/completed operation may switch modes within the same logical owner.
        const auto released = release(*selected, true);
        if (released != Result::success) {
            selected->status.state = State::failed;
            selected->status.result = released;
            return released;
        }
        retained = selected->lease;
    } else {
        for (; slot < sessions.size(); ++slot)
            if (sessions[slot].status.state == State::closed) {
                selected = &sessions[slot];
                break;
            }
    }
    if (!selected)
        return Result::busy;
    auto& s = *selected;
    s = {};
    s.lease = retained;
    s.config = config;
    if (++next_generation == 0U)
        ++next_generation;
    s.generation = next_generation;
    const bool needs_transport = config.source == Source::microphone || config.sink == Sink::output;
    Result result = retained.generation || !needs_transport
                        ? Result::success
                        : hardware(i2s::reserve(config.input, config.output, s.lease));
    if (result == Result::success && config.codec != Codec::none) {
        result = codec_factory ? codec_factory(config, encoding, s.codec) : Result::unsupported;
        if (result == Result::success && (!s.codec.process || !s.codec.close))
            result = Result::unsupported;
    }
    if (result == Result::success && config.source == Source::microphone) {
        auto wire = config.input;
        if (config.sink == Sink::output && same_clock(config.input, config.output)) {
            wire.dout = config.output.dout;
            if (config.output.mclk.valid())
                wire.mclk = config.output.mclk;
            if (config.output.controller >= 0)
                wire.controller = config.output.controller;
            s.paired = true;
        } else
            wire.dout = {};
        result = hardware(i2s::activate(s.lease, wire));
        if (result == Result::success) {
            s.input = s.lease;
            if (s.paired)
                s.output = s.input;
        }
    }
    if (result == Result::success && config.sink == Sink::output && !decoding && !s.paired)
        result = open_output(s, 0, 0);
    if (result == Result::success && config.source == Source::file)
        result = filesystem::open_reader(config.location.data(), s.reader) ==
                         filesystem::ReadResult::succeeded
                     ? Result::success
                     : Result::io_error;
    if (result == Result::success && config.source != Source::file &&
        config.source != Source::microphone && config.source != Source::text) {
        result = open_endpoint(config, true, s.source);
        if (result == Result::success && (!s.source.read || !s.source.close))
            result = Result::unsupported;
    }
    if (result == Result::success && config.sink != Sink::file && config.sink != Sink::output) {
        result = open_endpoint(config, false, s.sink);
        if (result == Result::success && (!s.sink.write || !s.sink.close))
            result = Result::unsupported;
    }
    if (result == Result::success && config.sink == Sink::file)
        result = filesystem::open_writer(config.location.data(), s.writer) ==
                         filesystem::MutationResult::succeeded
                     ? Result::success
                     : Result::io_error;
    if (result != Result::success) {
        if (release(s, retained.generation != 0) == Result::success && !retained.generation)
            s = {};
        else {
            s.status.state = State::failed;
            s.status.result = result;
            handle = {s.generation, static_cast<std::uint8_t>(slot)};
        }
        return result;
    }
    s.status.state = State::running;
    s.started_at = platform::monotonic_milliseconds();
    if (config.source == Source::microphone) {
        s.capture.reset(new (std::nothrow) Capture());
        if (s.capture && !s.capture->ring.allocate(kCaptureRingBytes))
            s.capture.reset();
        // Keep Tasmota's tested sample-rate-dependent Opus stack reservation.
        // Scale its variable portion for interleaved stereo; runtime watermarks
        // must qualify that extension on hardware. Other stages use the task floor.
        const auto stack_bytes =
            config.codec == Codec::opus
                ? kOpusCaptureStackBaseBytes +
                      config.input.sample_rate / 2U * platform::i2s::channel_count(config.input)
                : kCaptureTaskStackBytes;
        const auto period_ms = capture_period_milliseconds(config, s.codec);
        if (!s.capture || !platform::audio_worker::start(
                              s.capture->worker, capture_step, &s, stack_bytes, period_ms)) {
            if (release(s, retained.generation != 0) == Result::success && !retained.generation)
                s = {};
            else {
                s.status.state = State::failed;
                s.status.result = Result::no_memory;
                handle = {s.generation, static_cast<std::uint8_t>(slot)};
            }
            return Result::no_memory;
        }
    }
    handle = {s.generation, static_cast<std::uint8_t>(slot)};
    return Result::success;
}

Result configure(Handle h, const Config& config) {
    auto* s = resolve(h);
    if (!s)
        return Result::success;
    if (s->status.state == State::running)
        return Result::busy;
    if (s->lease.generation) {
        const auto checked = hardware(i2s::check_profiles(s->lease, config.input, config.output));
        if (checked != Result::success)
            return checked;
    }
    s->config = config;
    return Result::success;
}

Result finish(Handle h) {
    auto* s = resolve(h);
    if (s && s->status.state == State::running) {
        if (s->capture)
            s->capture->finishing.store(true, std::memory_order_release);
        else
            s->eof = true;
        s->config.loop = false;
    }
    return Result::success;
}

Result pause(Handle h, bool paused) {
    auto* s = resolve(h);
    if (!s || s->status.state != State::running)
        return Result::invalid_argument;
    if ((s->config.source != Source::file && s->config.source != Source::text) ||
        s->config.sink != Sink::output || !s->codec.process)
        return Result::unsupported;
    s->paused = paused;
    if (s->playback)
        s->playback->paused.store(paused, std::memory_order_relaxed);
    return Result::success;
}

Result stop(Handle h) {
    auto* s = resolve(h);
    if (!s)
        return Result::success;
    const auto result = release(*s);
    if (result == Result::success)
        *s = {};
    else {
        s->status.state = State::failed;
        s->status.result = result;
    }
    return result;
}

Status status(Handle h) {
    auto* s = resolve(h);
    if (!s)
        return {};
    auto result = s->status;
    if (s->playback) {
        result.output_bytes = s->playback->output_bytes.load(std::memory_order_relaxed);
        if (s->playback->worker.native)
            result.worker_stack_free_bytes =
                platform::audio_worker::stack_free_bytes(s->playback->worker);
    }
    if (s->capture) {
        result.input_bytes = s->capture->input_bytes.load(std::memory_order_relaxed);
        if (s->capture->worker.native)
            result.worker_stack_free_bytes =
                platform::audio_worker::stack_free_bytes(s->capture->worker);
    }
    if (result.state == State::running)
        result.elapsed_seconds = (platform::monotonic_milliseconds() - s->started_at) / 1000U;
    const auto* const format = s->playback  ? &s->config.output
                               : s->capture ? &s->config.input
                                            : nullptr;
    if (format) {
        result.sample_rate = format->sample_rate;
        result.channels = static_cast<std::uint8_t>(platform::i2s::channel_count(*format));
    }
    result.paused = s->paused;
    if (s->input.generation)
        result.input = i2s::status(s->input);
    if (s->output.generation)
        result.output = i2s::status(s->output);
    return result;
}

void poll() {
    for (auto& s : sessions)
        if (s.status.state == State::running) {
            if (s.capture)
                advance_capture(s);
            else if (s.config.sink == Sink::output && s.codec.process)
                advance_playback(s);
            else
                advance(s);
        }
}

Config& command_config() {
    return defaults;
}

Result start_command(const Config& c) {
    if (status(command).state == State::running)
        return Result::busy;
    return start(c, command);
}

Result stop_command() {
    const auto result = stop(command);
    if (result == Result::success)
        command = {};
    return result;
}

Result finish_command() {
    return finish(command);
}

Result pause_command(bool paused) {
    return pause(command, paused);
}

void set_output_gain(std::uint8_t percent) {
    gain_percent.store(std::min<std::uint8_t>(percent, 100), std::memory_order_relaxed);
}

std::uint8_t output_gain() {
    return gain_percent.load(std::memory_order_relaxed);
}

void set_input_gain(std::uint16_t q12_4) {
    input_gain_q12_4.store(std::min(q12_4, kMaximumInputGain), std::memory_order_relaxed);
}

std::uint16_t input_gain() {
    return input_gain_q12_4.load(std::memory_order_relaxed);
}

Status command_status() {
    return status(command);
}

const Config* command_session() {
    const auto* const s = resolve(command);
    return s ? &s->config : nullptr;
}

void set_stream_control(StreamControl control) {
    stream_callback = control;
}

Result stream_control(bool enabled) {
    if (!stream_callback)
        return Result::unsupported;
    const auto result = stream_callback(enabled);
    if (result == Result::success)
        streaming = enabled;
    return result;
}

bool stream_enabled() {
    return streaming;
}

const char* result_name(Result result) {
    switch (result) {
    case Result::success:
        return "Success";
    case Result::invalid_argument:
        return "InvalidArgument";
    case Result::unsupported:
        return "Unsupported";
    case Result::busy:
        return "Busy";
    case Result::no_memory:
        return "NoMemory";
    case Result::io_error:
        return "IoError";
    case Result::codec_error:
        return "CodecError";
    case Result::hardware_error:
        return "HardwareError";
    case Result::overrun:
        return "Overrun";
    }
    return "Failed";
}
} // namespace newmota::audio
