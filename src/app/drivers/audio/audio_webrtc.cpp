/*
  audio_webrtc.cpp - Microphone audio for the CSI camera's WebRTC track in Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_webrtc.cpp
 * @brief Ported from Tasmota's xdrv_81_4a_esp32_webcam_CSI_audio.ino.
 *
 * While the WebRTC peer streams, the configured I2S input runs at 8 kHz mono. A worker
 * collects 20 ms frames, removes DC like Tasmota's filter, and sends them as Opus (with
 * `USE_AUDIO_OPUS`, 16 kbit/s VOIP) or as G.711 A-law. Without `USE_I2S` the answer still
 * offers the audio track, but nothing is sent, as in Tasmota.
 */
#include "drivers/audio/audio_webrtc.h"

#include "newmota_features.h"
#include "newmota/platform/camera_csi.h"

#if USE_I2S
#include "drivers/audio/audio_session.h"
#include "drivers/audio/i2s_service.h"
#include "newmota/log.h"
#include "newmota/platform/audio_worker.h"
#if USE_AUDIO_OPUS
#include "libopus/opus.h"
#endif

#include <array>
#include <cstring>
#endif

namespace newmota::camera {
#if USE_I2S
namespace {
constexpr std::uint32_t kSampleRate = 8000;
constexpr std::size_t kFrameSamples = 160; // 20 ms
constexpr std::uint32_t kStepMs = 5;
#if USE_AUDIO_OPUS
constexpr std::size_t kStack = 24576; // Opus encodes on the worker stack
constexpr int kOpusMaxPacket = 256;
#else
constexpr std::size_t kStack = 4096;
#endif

struct State final {
    i2s::Handle handle{};
    platform::audio_worker::Worker worker{};
    bool open{}, unavailable{};
    std::array<std::int16_t, kFrameSamples> pcm{};
    std::size_t filled{};
    std::int32_t dc_x{}, dc_y{};
    bool dc_primed{};
#if USE_AUDIO_OPUS
    OpusEncoder* opus{};
#endif
};

State audio{};

#if !USE_AUDIO_OPUS
/** @brief ITU-T G.711 A-law, Tasmota's encoder. */
std::uint8_t alaw(std::int16_t pcm) {
    static constexpr std::uint8_t kSegments[128] = {1, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4,
        5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
        6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};
    const int sign = ((~pcm) >> 8) & 0x80;
    int value = sign == 0 ? -pcm : pcm;
    if (value > 32635)
        value = 32635;
    std::uint8_t code;
    if (value >= 256) {
        const int exponent = kSegments[(value >> 8) & 0x7F];
        code = static_cast<std::uint8_t>((exponent << 4) | ((value >> (exponent + 3)) & 0x0F));
    } else {
        code = static_cast<std::uint8_t>(value >> 4);
    }
    return static_cast<std::uint8_t>(code ^ (sign ^ 0x55));
}
#endif

/** @brief Removes DC: y[n] = x[n] - x[n-1] + 0.9921875 y[n-1], as Tasmota does. */
void remove_dc() {
    if (!audio.dc_primed) {
        std::int32_t sum{};
        for (const auto sample : audio.pcm)
            sum += sample;
        audio.dc_x = sum / static_cast<std::int32_t>(kFrameSamples);
        audio.dc_y = 0;
        audio.dc_primed = true;
    }
    for (auto& sample : audio.pcm) {
        const std::int32_t x = sample;
        const std::int32_t y = x - audio.dc_x + (audio.dc_y * 32511) / 32768;
        audio.dc_x = x;
        audio.dc_y = y;
        sample = static_cast<std::int16_t>(y > 32767 ? 32767 : (y < -32768 ? -32768 : y));
    }
}

void send_frame() {
    remove_dc();
#if USE_AUDIO_OPUS
    std::uint8_t packet[kOpusMaxPacket];
    const auto size =
        opus_encode(audio.opus, audio.pcm.data(), int(kFrameSamples), packet, kOpusMaxPacket);
    if (size > 0)
        (void)platform::camera_csi::webrtc_send_audio(packet, static_cast<std::size_t>(size));
#else
    std::uint8_t packet[kFrameSamples];
    for (std::size_t i = 0; i < kFrameSamples; ++i)
        packet[i] = alaw(audio.pcm[i]);
    (void)platform::camera_csi::webrtc_send_audio(packet, sizeof(packet));
#endif
}

/** @brief Collects captured samples and sends every complete 20 ms frame. */
void step(void*) {
    for (;;) {
        auto* target = reinterpret_cast<std::uint8_t*>(audio.pcm.data() + audio.filled);
        const auto wanted = (kFrameSamples - audio.filled) * sizeof(std::int16_t);
        const auto transfer = i2s::read(audio.handle, target, wanted);
        if (transfer.result != i2s::Result::success || transfer.count == 0U)
            return;
        audio.filled += transfer.count / sizeof(std::int16_t);
        if (audio.filled < kFrameSamples)
            return;
        send_frame();
        audio.filled = 0;
    }
}

bool start() {
    if (i2s::Config wired{}; !i2s::configured_config(wired, platform::i2s::Direction::receive)) {
        AddLog(LogLevel::info, "CAM: no I2S microphone for WebRTC audio");
        return false;
    }
    auto config = audio::command_config().input; // the I2SConfig Rx profile over the template
    config.sample_rate = kSampleRate;
    config.slot_mask = platform::i2s::kLeftSlotMask; // one microphone, mono
    config.sample_bits = platform::i2s::SampleWidth::bits16;
    if (i2s::open(config, audio.handle) != i2s::Result::success) {
        AddLog(LogLevel::error, "CAM: I2S microphone busy or unavailable");
        return false;
    }
#if USE_AUDIO_OPUS
    int error{};
    audio.opus = opus_encoder_create(int(kSampleRate), 1, OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK || !audio.opus) {
        (void)i2s::close(audio.handle);
        audio.opus = nullptr;
        return false;
    }
    (void)opus_encoder_ctl(audio.opus, OPUS_SET_COMPLEXITY(1));
    (void)opus_encoder_ctl(audio.opus, OPUS_SET_BITRATE(16000));
#endif
    audio.filled = 0;
    audio.dc_primed = false;
    if (!platform::audio_worker::start(audio.worker, step, nullptr, kStack, kStepMs)) {
        stop_audio();
        return false;
    }
    audio.open = true;
    AddLog(LogLevel::info, "CAM: WebRTC audio from I2S at 8 kHz");
    return true;
}
} // namespace

void poll_audio() {
    const bool wanted = platform::camera_csi::webrtc_streaming();
    if (wanted && !audio.open && !audio.unavailable)
        audio.unavailable = !start();
    else if (!wanted) {
        if (audio.open)
            stop_audio();
        audio.unavailable = false; // try again with the next peer
    }
}

void stop_audio() {
    platform::audio_worker::stop(audio.worker);
    if (audio.open || audio.handle.generation)
        (void)i2s::close(audio.handle);
#if USE_AUDIO_OPUS
    if (audio.opus)
        opus_encoder_destroy(audio.opus);
    audio.opus = nullptr;
#endif
    audio.handle = {};
    audio.open = false;
}
#else
void poll_audio() {}

void stop_audio() {}
#endif
} // namespace newmota::camera
