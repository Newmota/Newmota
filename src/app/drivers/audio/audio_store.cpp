/*
  audio_store.cpp - Persistent I2SConfig profiles in the audio driver's own file

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_store.cpp
 * @brief Fixed little-endian payload of two 23-byte profiles, transmit first.
 */
#include "drivers/audio/audio_store.h"

namespace newmota::audio {
namespace {
AudioSettings cached;

void write_u16(std::uint8_t* out, std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>(value);
    out[1] = static_cast<std::uint8_t>(value >> 8U);
}

std::uint16_t read_u16(const std::uint8_t* in) {
    return static_cast<std::uint16_t>(in[0] | in[1] << 8U);
}

void encode_profile(std::uint8_t* out, const I2sProfileSettings& p) {
    out[0] = p.present ? 1U : 0U;
    out[1] = p.mode;
    out[2] = p.role;
    out[3] = p.msb_right;
    out[4] = p.pdm_pcm ? 1U : 0U;
    out[5] = p.sample_bits;
    out[6] = p.slot_bits;
    write_u16(out + 7, static_cast<std::uint16_t>(p.sample_rate));
    write_u16(out + 9, static_cast<std::uint16_t>(p.sample_rate >> 16U));
    write_u16(out + 11, p.slot_mask);
    write_u16(out + 13, p.dma_frames);
    write_u16(out + 15, p.pdm_clock);
    out[17] = p.framing;
    write_u16(out + 18, p.gain);
    out[20] = p.ws_width;
    out[21] = p.tuning_flags;
    out[22] = p.dma_descriptors;
}

/** @brief False when a flag is not 0/1 or a present profile is invalid. */
bool decode_profile(const std::uint8_t* in, I2sProfileSettings& p) {
    if (in[0] > 1U || in[4] > 1U)
        return false;
    p.present = in[0] != 0U;
    p.mode = in[1];
    p.role = in[2];
    p.msb_right = in[3];
    p.pdm_pcm = in[4] != 0U;
    p.sample_bits = in[5];
    p.slot_bits = in[6];
    p.sample_rate = static_cast<std::uint32_t>(read_u16(in + 7)) |
                    static_cast<std::uint32_t>(read_u16(in + 9)) << 16U;
    p.slot_mask = read_u16(in + 11);
    p.dma_frames = read_u16(in + 13);
    p.pdm_clock = read_u16(in + 15);
    p.framing = in[17];
    p.gain = read_u16(in + 18);
    p.ws_width = in[20];
    p.tuning_flags = in[21];
    p.dma_descriptors = in[22];
    return !p.present || p.valid();
}

using Payload = std::array<std::uint8_t, AudioSettings::kRecordSize>;

bool decode(const std::uint8_t* payload, AudioSettings& settings) {
    AudioSettings decoded;
    if (!decode_profile(payload, decoded.transmit) ||
        !decode_profile(payload + AudioSettings::kProfileEncodedSize, decoded.receive) ||
        !decoded.valid())
        return false;
    settings = decoded;
    return true;
}

/** @brief A CRC-clean payload with out-of-range values is treated as corrupt. */
bool valid_payload(const std::uint8_t* payload) {
    AudioSettings scratch;
    return decode(payload, scratch);
}

constexpr DriverSettingsSpec kSpec{"/.newmota/audio.bin", "/.newmota/audio.tmp",
    "/.newmota/audio.prev", {'N', 'M', 'A', 'U'}, 2, AudioSettings::kRecordSize, valid_payload};

Payload encode(const AudioSettings& settings) {
    Payload payload{};
    encode_profile(payload.data(), settings.transmit);
    encode_profile(payload.data() + AudioSettings::kProfileEncodedSize, settings.receive);
    return payload;
}

} // namespace

DriverSettingsState initialize_store() {
    auto payload = encode({});
    const auto state = load_driver_settings(kSpec, payload.data());
    if (!decode(payload.data(), cached))
        cached = {};
    return state;
}

const AudioSettings& stored_settings() {
    return cached;
}

bool store_settings(const AudioSettings& settings) {
    if (!settings.valid() || !save_driver_settings(kSpec, encode(settings).data()))
        return false;
    cached = settings;
    return true;
}
} // namespace newmota::audio
