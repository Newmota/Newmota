/*
  audio_web.cpp - browser view of the command audio session

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file audio_web.cpp
 * @brief Maps the command audio session to the Home player contract.
 */

#include "services/web/audio_web.h"

#include "core/command/command.h"
#include "drivers/audio/audio_session.h"
#if USE_AUDIO_MP3 || USE_AUDIO_AAC || USE_AUDIO_OPUS
#include "drivers/audio/audio_network.h"
#endif

namespace newmota::audio_web {
namespace {

struct PlaybackCodec {
    bool built;
    const char* name;
};

/** Playback decoders in Tasmota's selector order (0 AAC, 1 MP3, 2 Opus); the page picks N. */
constexpr PlaybackCodec kPlaybackCodecs[] = {
    {USE_AUDIO_AAC != 0, "aac"},
    {USE_AUDIO_MP3 != 0, "mp3"},
    {USE_AUDIO_OPUS != 0, "opus"},
};

const char* state_name(const audio::State state) {
    switch (state) {
    case audio::State::closed:
        return "idle";
    case audio::State::running:
        return "running";
    case audio::State::ended:
        return "ended";
    case audio::State::failed:
        return "failed";
    }
    return "idle";
}

const char* source_name(const audio::Source source) {
    switch (source) {
    case audio::Source::microphone:
        return "microphone";
    case audio::Source::file:
        return "file";
    case audio::Source::network:
        return "network";
    case audio::Source::custom:
        return "custom";
    case audio::Source::text:
        return "text";
    }
    return "custom";
}

const char* codec_name(const audio::Codec codec) {
    switch (codec) {
    case audio::Codec::none:
        return "none";
    case audio::Codec::mp3:
        return "mp3";
    case audio::Codec::aac:
        return "aac";
    case audio::Codec::opus:
        return "opus";
    case audio::Codec::sam:
        return "sam";
    case audio::Codec::tone:
        return "tone";
    }
    return "none";
}

} // namespace

bool write_player(CommandOutput& output) {
    const auto status = audio::command_status();
    const auto* const session = audio::command_session();
    if (!output.write("{\"state\":") || !output.json_string(state_name(status.state)))
        return false;
    if (session != nullptr) {
        if (!output.write(",\"source\":") || !output.json_string(source_name(session->source)) ||
            !output.write(",\"record\":") ||
            !output.write(session->sink == audio::Sink::file ? "true" : "false") ||
            !output.write(",\"codec\":") || !output.json_string(codec_name(session->codec)) ||
            !output.write(",\"location\":") || !output.json_string(session->location.data()) ||
            !output.format(",\"loop\":%s,\"paused\":%s,\"elapsed\":%lu,\"rate\":%lu,"
                           "\"channels\":%u",
                session->loop ? "true" : "false", status.paused ? "true" : "false",
                static_cast<unsigned long>(status.elapsed_seconds),
                static_cast<unsigned long>(status.sample_rate), unsigned(status.channels)))
            return false;
        if (status.state == audio::State::failed &&
            (!output.write(",\"result\":") ||
                !output.json_string(audio::result_name(status.result))))
            return false;
#if USE_AUDIO_MP3 || USE_AUDIO_AAC || USE_AUDIO_OPUS
        if (session->source == audio::Source::network &&
            (!output.write(",\"station\":") || !output.json_string(audio::network_station()) ||
                !output.write(",\"title\":") || !output.json_string(audio::network_title())))
            return false;
#endif
    }
    if (!output.format(",\"gain\":%u,\"codecs\":[", unsigned(audio::output_gain())))
        return false;
    bool first = true;
    for (const auto& codec : kPlaybackCodecs) {
        if (!codec.built)
            continue;
        if ((!first && !output.write(",")) || !output.json_string(codec.name))
            return false;
        first = false;
    }
    return output.write("]}");
}

} // namespace newmota::audio_web
