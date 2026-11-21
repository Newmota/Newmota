/*
  audio_session.h - Bounded native audio sessions

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_session.h
 * @brief Connects native audio endpoints without moving samples through Berry.
 */
#pragma once
#include "drivers/audio/i2s_service.h"
#include <array>
#include <string_view>

namespace newmota::audio {
inline constexpr std::size_t kSessionCount = 2;
inline constexpr std::size_t kBlockBytes = 4096;
/** @brief Storage bytes reserved for a null-terminated endpoint location. */
inline constexpr std::size_t kAudioLocationBytes = 320;
/** @brief Maximum number of optional endpoint providers registered at startup. */
inline constexpr std::size_t kMaximumEndpointProviders = 4;
enum class Source : std::uint8_t { microphone, file, network, custom, text };
enum class Sink : std::uint8_t { output, file, network, custom };
enum class Codec : std::uint8_t { none, mp3, aac, opus, sam, tone };
enum class Result : std::uint8_t {
    success,
    invalid_argument,
    unsupported,
    busy,
    no_memory,
    io_error,
    codec_error,
    hardware_error,
    overrun
};
enum class State : std::uint8_t { closed, running, ended, failed };

struct Handle {
    std::uint32_t generation{};
    std::uint8_t slot{};
};

/** @brief Complete session intent. File/network paths contain encoded audio.
 * `none` is reserved for native live processing, not an uncompressed file format.
 * `text` and `sam` pair: the location is the text and SAM speaks it to the output.
 * `text` and `tone` pair too: the generator plays until finished and ignores the location.
 * A codec changes output's sample rate and slot count to match decoded audio.
 */
struct Config {
    Source source{Source::microphone};
    Sink sink{Sink::output};
    Codec codec{Codec::none};
    i2s::Config input{}, output{};
    std::array<char, kAudioLocationBytes> location{};
    bool loop{};
};

struct Status {
    State state{State::closed};
    Result result{Result::success};
    std::uint32_t input_bytes{}, output_bytes{}, loops{}, elapsed_seconds{};
    platform::i2s::Status input{}, output{};
    std::uint32_t worker_stack_free_bytes{};
    /** @brief Sample format of the open I2S side: decoded playback, else microphone capture.
     * Zero while playback still waits for the first decoded frames.
     */
    std::uint32_t sample_rate{};
    std::uint8_t channels{};
    bool paused{};
};

/** @brief Bounded endpoint progress; EOF is distinct from temporary starvation. */
struct Io {
    Result result{Result::success};
    std::size_t count{};
    bool eof{};
};

/** @brief Optional native network/BLE/processing endpoint. Callbacks never retain buffers.
 * close is required and must release only this endpoint's resources. An open failure
 * must unwind its own partial allocations. Input read and output write are nonblocking.
 */
struct Endpoint {
    void* context{};
    Io (*read)(void*, std::uint8_t*, std::size_t){};
    Io (*write)(void*, const std::uint8_t*, std::size_t){};
    void (*close)(void*){};
};

/** @brief Codec output describes complete interleaved signed 16-bit sample frames.
 * For encoding, bytes are compressed and format is ignored. EOF calls finish until
 * finished=true. reset restarts a decoder for bounded file loops.
 */
struct Step {
    Result result{Result::success};
    std::size_t consumed{}, produced{};
    std::uint32_t sample_rate{};
    std::uint8_t channels{};
    bool finished{};
};

struct Stage {
    void* context{};
    Step (*process)(void*, const std::uint8_t*, std::size_t, bool, std::uint8_t*, std::size_t){};
    Result (*reset)(void*){};
    void (*close)(void*){};
    /** @brief Encoder only: sample frames per channel consumed by one encoded packet. The
     * capture worker aligns its wake period to this (bounded by the DMA depth), as
     * Tasmota's recording task does. Zero means the stage has no preferred pass.
     */
    std::uint32_t pass_frames{};
};

/** @brief Creates a selected encoder or decoder. Preserve carried codec source tuning. */
using CodecFactory = Result (*)(const Config&, bool encode, Stage&);
/** @brief Creates a non-file/non-I2S endpoint; used for native networking and processing. */
using EndpointFactory = Result (*)(const Config&, bool input, Endpoint&);
/** @brief Installs compiled native providers before sessions start. */
void set_providers(CodecFactory, EndpointFactory);
/** @brief Adds a native endpoint provider; unsupported routes fall through. */
bool register_endpoint_provider(EndpointFactory);
/** @brief Starts atomically, rolling back every successfully acquired resource on failure. */
Result start(const Config&, Handle&);
/** @brief Updates a completed session's intent on its retained routes.
 * New pads/controllers require close first; rejected changes preserve ownership.
 */
Result configure(Handle, const Config&);
/** @brief Requests capture stop and encoder finalization; app polling drains pending output. */
Result finish(Handle);
/** @brief Pauses or resumes decoded file playback; the DMA drains and then stays silent.
 * @return Unsupported unless the session decodes a file to I2S, InvalidArgument if it is
 * not running.
 */
Result pause(Handle, bool paused);
/** @brief Aborts and releases a session; hardware cleanup failures retain its lease. */
Result stop(Handle);
/** @brief Reports a live or completed generation; stale handles report closed. */
Status status(Handle);
/** @brief Performs one bounded read/codec/write step per active session. */
void poll();
/** @brief Returns the command-owned session configuration used by command and Berry setup. */
Config& command_config();
/** @brief Runs one command-owned session without interrupting other session owners. */
Result start_command(const Config&);
Result stop_command();
Result finish_command();
Result pause_command(bool paused);
Status command_status();
/** @brief The command session's intent while it exists; null after stop. Application task only.
 */
const Config* command_session();
/** @brief Sets the software gain (0-100 percent) applied to decoded playback PCM.
 * It is volatile, takes effect within the playback buffer depth, and does not affect
 * raw live routes.
 */
void set_output_gain(std::uint8_t percent);
std::uint8_t output_gain();
/** @brief Sets the microphone gain in Q12.4 (16 is unity, Tasmota's Rx Gain) applied to
 * captured PCM before an encoder, with clipping. Raw live routes stay unscaled.
 */
void set_input_gain(std::uint16_t q12_4);
std::uint16_t input_gain();
/** @brief Enables the native HTTP microphone streaming provider (Tasmota I2SStream). */
using StreamControl = Result (*)(bool);
void set_stream_control(StreamControl);
Result stream_control(bool);
bool stream_enabled();
/** @brief Stable error spelling for commands and native language bindings. */
const char* result_name(Result);
} // namespace newmota::audio
