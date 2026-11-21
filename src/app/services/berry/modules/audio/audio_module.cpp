/*
  audio_module.cpp - Berry native audio session bindings

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_module.cpp
 * @brief Berry configures native input/output sessions; audio stays outside the VM.
 */
#include "services/berry/modules/audio/audio_module.h"
#include "drivers/audio/audio_session.h"
#include "drivers/audio/audio_level.h"
#include "drivers/audio/audio_spectrum.h"
#include "drivers/audio/audio_tone.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include <utility>
extern "C" {
#include "berry.h"
}

namespace {
namespace audio = newmota::audio;
namespace i2s = newmota::platform::i2s;

namespace config_limits {
constexpr std::int32_t kNoController = -1;
constexpr std::int32_t kMinimumSampleRateHz = 8000;
constexpr std::int32_t kMaximumSampleRateHz = i2s::kMaximumPcmRateHz;
constexpr std::int32_t kMinimumRawPdmRateHz = 1;
constexpr std::int32_t kMaximumRawPdmRateHz = i2s::kMaximumRawPdmRateHz;
constexpr std::int32_t kMinimumSlotCount = 1;
constexpr std::int32_t kMaximumSlotCount = 16;
constexpr std::int32_t kMinimumSlotMask = i2s::kLeftSlotMask;
constexpr std::int32_t kMaximumSlotMask = (1U << kMaximumSlotCount) - 1U;
constexpr std::int32_t kUnassignedPin = -1;
constexpr std::int32_t kMaximumPinId = newmota::PinId::kInvalid - 1;
} // namespace config_limits

namespace config_values {
constexpr std::array<std::pair<const char*, i2s::Mode>, 3> kModes{{
    {"standard", i2s::Mode::standard},
    {"tdm", i2s::Mode::tdm},
    {"pdm", i2s::Mode::pdm},
}};
constexpr std::array<std::pair<const char*, i2s::Framing>, 4> kFraming{{
    {"philips", i2s::Framing::philips},
    {"left", i2s::Framing::left_justified},
    {"right", i2s::Framing::right_justified},
    {"pcm", i2s::Framing::pcm_short},
}};
constexpr std::array<std::pair<const char*, i2s::Role>, 2> kRoles{{
    {"controller", i2s::Role::controller},
    {"target", i2s::Role::target},
}};
constexpr std::array<std::pair<const char*, i2s::MsbRight>, 3> kMsbRight{{
    {"automatic", i2s::MsbRight::automatic},
    {"disabled", i2s::MsbRight::disabled},
    {"enabled", i2s::MsbRight::enabled},
}};
constexpr std::array<std::pair<const char*, audio::Source>, 5> kSources{{
    {"microphone", audio::Source::microphone},
    {"file", audio::Source::file},
    {"network", audio::Source::network},
    {"custom", audio::Source::custom},
    {"text", audio::Source::text},
}};
constexpr std::array<std::pair<const char*, audio::Sink>, 4> kSinks{{
    {"output", audio::Sink::output},
    {"file", audio::Sink::file},
    {"network", audio::Sink::network},
    {"custom", audio::Sink::custom},
}};
constexpr std::array<std::pair<const char*, audio::Codec>, 6> kCodecs{{
    {"none", audio::Codec::none},
    {"mp3", audio::Codec::mp3},
    {"aac", audio::Codec::aac},
    {"opus", audio::Codec::opus},
    {"sam", audio::Codec::sam},
    {"tone", audio::Codec::tone},
}};
} // namespace config_values

struct Instance {
    std::uint32_t id{};
    audio::Config config{};
    audio::Handle handle{};
};

std::array<Instance, audio::kSessionCount> instances{};
std::uint32_t generation{};

void check(bvm* vm, audio::Result result) {
    if (result != audio::Result::success)
        be_raise(vm, "runtime_error", audio::result_name(result));
}

bool member(bvm* vm, int map, const char* name) {
    if (!be_getmember(vm, map, ".p")) {
        be_pop(vm, 1);
        return false;
    }
    const auto data = be_absindex(vm, -1);
    be_pushstring(vm, name);
    const bool found = be_getindex(vm, data);
    be_remove(vm, -2);
    be_remove(vm, -2);
    if (!found)
        be_pop(vm, 1);
    return found;
}

std::int32_t number(bvm* vm, int map, const char* name, std::int32_t fallback, std::int32_t low,
    std::int32_t high) {
    if (!member(vm, map, name))
        return fallback;
    if (!be_isint(vm, -1))
        be_raise(vm, "type_error", "audio configuration expects integers");
    const auto value = be_toint(vm, -1);
    if (value < low || value > high)
        be_raise(vm, "value_error", "audio integer out of range");
    be_pop(vm, 1);
    return static_cast<std::int32_t>(value);
}

template <typename Value, std::size_t Size>
Value choice(bvm* vm, int map, const char* name, Value fallback,
    const std::array<std::pair<const char*, Value>, Size>& values) {
    if (!member(vm, map, name))
        return fallback;
    if (!be_isstring(vm, -1))
        be_raise(vm, "type_error", "audio configuration expects strings");
    const auto* value = be_tostring(vm, -1);
    for (const auto& [name, selected] : values)
        if (std::strcmp(value, name) == 0) {
            be_pop(vm, 1);
            return selected;
        }
    be_raise(vm, "value_error", "unknown audio configuration value");
    return fallback;
}

i2s::SampleWidth sample_width(bvm* vm, int map, const char* name, i2s::SampleWidth fallback) {
    if (!member(vm, map, name))
        return fallback;
    if (!be_isint(vm, -1))
        be_raise(vm, "type_error", "audio sample width expects an integer");
    const auto value = be_toint(vm, -1);
    be_pop(vm, 1);
    switch (value) {
    case static_cast<bint>(i2s::SampleWidth::bits16):
        return i2s::SampleWidth::bits16;
    case static_cast<bint>(i2s::SampleWidth::bits24):
        return i2s::SampleWidth::bits24;
    case static_cast<bint>(i2s::SampleWidth::bits32):
        return i2s::SampleWidth::bits32;
    default:
        be_raise(vm, "value_error", "audio sample width must be 16, 24 or 32 bits");
        return fallback;
    }
}

void wire(bvm* vm, int map, newmota::i2s::Config& config) {
    using namespace config_limits;
    const auto controller_count = i2s::capabilities().controllers;
    const auto maximum_controller =
        controller_count ? static_cast<std::int32_t>(controller_count - 1U) : kNoController;
    config.controller = static_cast<std::int8_t>(
        number(vm, map, "controller", config.controller, kNoController, maximum_controller));
    config.sample_bits = sample_width(vm, map, "bits", config.sample_bits);
    config.slot_bits = sample_width(vm, map, "slot_bits", config.sample_bits);
    config.slots = static_cast<std::uint8_t>(
        number(vm, map, "slots", config.slots, kMinimumSlotCount, kMaximumSlotCount));
    config.slot_mask = static_cast<std::uint32_t>(
        number(vm, map, "slot_mask", config.slot_mask, kMinimumSlotMask, kMaximumSlotMask));
    config.mode = choice(vm, map, "mode", config.mode, config_values::kModes);
    config.framing = choice(vm, map, "framing", config.framing, config_values::kFraming);
    config.role = choice(vm, map, "role", config.role, config_values::kRoles);
    config.msb_right = choice(vm, map, "msb_right", config.msb_right, config_values::kMsbRight);
    config.dma_frames = static_cast<std::uint16_t>(
        number(vm, map, "dma_frames", config.dma_frames, 1, i2s::kMaximumDmaFrames));
    const auto pin = [&](const char* name, newmota::PinId current) {
        const auto fallback = current.valid() ? current.value() : kUnassignedPin;
        const auto p = number(vm, map, name, fallback, kUnassignedPin, kMaximumPinId);
        return p < 0 ? newmota::PinId{} : newmota::PinId{static_cast<std::uint16_t>(p)};
    };
    config.bclk = pin("bclk", config.bclk);
    config.ws = pin("ws", config.ws);
    config.mclk = pin("mclk", config.mclk);
    config.pdm_clock = pin("pdm_clock", config.pdm_clock);
    if (member(vm, map, "pdm_pcm")) {
        if (!be_isbool(vm, -1))
            be_raise(vm, "type_error", "pdm_pcm expects bool");
        config.pdm_pcm = be_tobool(vm, -1);
        be_pop(vm, 1);
    }
    const auto raw_pdm = config.mode == i2s::Mode::pdm && !config.pdm_pcm;
    const auto min_rate = raw_pdm ? kMinimumRawPdmRateHz : kMinimumSampleRateHz;
    const auto max_rate = raw_pdm ? kMaximumRawPdmRateHz : kMaximumSampleRateHz;
    config.sample_rate =
        static_cast<std::uint32_t>(number(vm, map, "rate", config.sample_rate, min_rate, max_rate));
    config.din = pin("din", config.din);
    config.dout = pin("dout", config.dout);
}

Instance* instance(bvm* vm) {
    be_getmember(vm, 1, ".id");
    const auto id = be_isint(vm, -1) ? static_cast<std::uint32_t>(be_toint(vm, -1)) : 0U;
    be_pop(vm, 1);
    for (auto& candidate : instances)
        if (id && candidate.id == id)
            return &candidate;
    return nullptr;
}

/** @brief Updates explicit direction profiles; absent fields retain their values. */
void configuration(bvm* vm, audio::Config& config) {
    config.source = choice(vm, 2, "source", config.source, config_values::kSources);
    config.sink = choice(vm, 2, "sink", config.sink, config_values::kSinks);
    config.codec = choice(vm, 2, "codec", config.codec, config_values::kCodecs);
    if (member(vm, 2, "location")) {
        if (!be_isstring(vm, -1))
            be_raise(vm, "type_error", "audio location expects string");
        const auto length = static_cast<std::size_t>(be_strlen(vm, -1));
        if (length >= config.location.size() || std::memchr(be_tostring(vm, -1), 0, length))
            be_raise(vm, "value_error", "invalid audio location");
        config.location.fill(0);
        std::copy_n(be_tostring(vm, -1), length, config.location.data());
        be_pop(vm, 1);
    }
    if (member(vm, 2, "loop")) {
        if (!be_isbool(vm, -1))
            be_raise(vm, "type_error", "audio loop expects bool");
        config.loop = be_tobool(vm, -1);
        be_pop(vm, 1);
    }
    if (member(vm, 2, "input")) {
        if (!be_ismapinstance(vm, -1))
            be_raise(vm, "type_error", "audio input expects configuration map");
        wire(vm, be_absindex(vm, -1), config.input);
        config.input.dout = {};
        be_pop(vm, 1);
    }
    if (member(vm, 2, "output")) {
        if (!be_ismapinstance(vm, -1))
            be_raise(vm, "type_error", "audio output expects configuration map");
        wire(vm, be_absindex(vm, -1), config.output);
        config.output.din = {};
        be_pop(vm, 1);
    }
}

int Configure(bvm* vm) {
    auto* s = instance(vm);
    if (!s || be_top(vm) != 2 || !be_ismapinstance(vm, 2))
        be_raise(vm, "type_error", "configure expects an audio configuration map");
    if (audio::status(s->handle).state == audio::State::running)
        be_raise(vm, "runtime_error", "finish the audio operation before configuring it");
    auto config = s->config;
    configuration(vm, config);
    check(vm, audio::configure(s->handle, config));
    s->config = config;
    be_return_nil(vm);
}

int Init(bvm* vm) {
    if (be_top(vm) != 2 || !be_ismapinstance(vm, 2))
        be_raise(vm, "type_error", "audio.Session(configuration_map)");
    if (instance(vm))
        be_raise(vm, "runtime_error", "audio session already initialized");
    // Start from the I2SConfig profiles, so board settings apply to Berry sessions too.
    audio::Config config{};
    config.input = audio::command_config().input;
    config.output = audio::command_config().output;
    configuration(vm, config);
    for (auto& candidate : instances)
        if (!candidate.id) {
            if (++generation == 0)
                ++generation;
            candidate = {generation, config, {}};
            be_pushint(vm, static_cast<bint>(generation));
            be_setmember(vm, 1, ".id");
            be_pop(vm, 1);
            be_return_nil(vm);
        }
    be_raise(vm, "runtime_error", "audio session capacity exhausted");
    be_return_nil(vm);
}

int Start(bvm* vm) {
    auto* s = instance(vm);
    if (!s)
        be_raise(vm, "runtime_error", "closed audio session");
    if (audio::status(s->handle).state == audio::State::running)
        be_raise(vm, "runtime_error", "audio session already running");
    check(vm, audio::start(s->config, s->handle));
    be_return_nil(vm);
}

int Stop(bvm* vm) {
    auto* s = instance(vm);
    if (s) {
        check(vm, audio::finish(s->handle));
    }
    be_return_nil(vm);
}

int Close(bvm* vm) {
    auto* s = instance(vm);
    if (s) {
        check(vm, audio::stop(s->handle));
        *s = {};
    }
    be_return_nil(vm);
}

int Deinit(bvm* vm) {
    auto* s = instance(vm);
    if (s && audio::stop(s->handle) == audio::Result::success)
        *s = {};
    be_return_nil(vm);
}

void field(bvm* vm, const char* key, std::uint32_t value) {
    be_pushstring(vm, key);
    be_pushint(vm, static_cast<bint>(value));
    be_data_insert(vm, -3);
    be_pop(vm, 2);
}

int Status(bvm* vm) {
    auto* s = instance(vm);
    if (!s)
        be_return_nil(vm);
    const auto status = audio::status(s->handle);
    be_newobject(vm, "map");
    field(vm, "state", static_cast<unsigned>(status.state));
    field(vm, "input_bytes", status.input_bytes);
    field(vm, "output_bytes", status.output_bytes);
    field(vm, "loops", status.loops);
    field(vm, "elapsed_seconds", status.elapsed_seconds);
    field(vm, "overruns", status.input.overruns);
    field(vm, "worker_stack_free_bytes", status.worker_stack_free_bytes);
    field(vm, "underruns", status.output.underruns);
    be_pushstring(vm, "result");
    be_pushstring(vm, audio::result_name(status.result));
    be_data_insert(vm, -3);
    be_pop(vm, 2);
    be_pop(vm, 1);
    be_return(vm);
}

/** @brief tone(hz [, volume]) retunes a running `tone` session; 0 Hz is silence. */
int Tone(bvm* vm) {
    auto* s = instance(vm);
    if (!s || s->config.codec != audio::Codec::tone ||
        audio::status(s->handle).state != audio::State::running)
        be_raise(vm, "runtime_error", "tone needs a running tone session");
    const auto top = be_top(vm);
    if (top < 2 || top > 3 || !be_isint(vm, 2) || (top == 3 && !be_isint(vm, 3)))
        be_raise(vm, "type_error", "tone(hz [, volume])");
    const auto hz = be_toint(vm, 2);
    const auto volume = top == 3 ? be_toint(vm, 3) : 100;
    if (hz < 0 || hz > static_cast<bint>(audio::kToneMaximumHz) || volume < 0 || volume > 100)
        be_raise(vm, "value_error", "tone hz is 0..24000 and volume 0..100");
    check(vm, audio::set_tone(static_cast<std::uint32_t>(hz), static_cast<std::uint32_t>(volume)));
    be_return_nil(vm);
}

/** @brief Inserts a real number; the target map is at the top of the stack. */
void real_field(bvm* vm, const char* key, double value) {
    be_pushstring(vm, key);
    be_pushreal(vm, static_cast<breal>(value));
    be_data_insert(vm, -3);
    be_pop(vm, 2);
}

/** @brief Finishes `key -> list`: the key and the new list sit above the target map. */
void end_list_field(bvm* vm) {
    be_pop(vm, 1);
    be_data_insert(vm, -3);
    be_pop(vm, 2);
}

/** @brief Tenths of a dB as a real dB value. */
double db(std::int32_t tenths) {
    return static_cast<double>(tenths) / 10.0;
}

/** @brief Raises for a failed native call; the analyzers report `Result` names. */
void checked(bvm* vm, audio::Result result) {
    if (result != audio::Result::success)
        be_raise(vm, "runtime_error", audio::result_name(result));
}

int LevelStart(bvm* vm) {
    checked(vm, audio::level_start());
    be_return_nil(vm);
}

int LevelStop(bvm* vm) {
    checked(vm, audio::level_stop());
    be_return_nil(vm);
}

int LevelReset(bvm* vm) {
    audio::level_reset();
    be_return_nil(vm);
}

/** @brief offset(dB) adds a calibration offset to every reported level. */
int LevelOffset(bvm* vm) {
    if (be_top(vm) != 2 || !be_isnumber(vm, 2))
        be_raise(vm, "type_error", "offset(dB)");
    audio::level_set_offset(static_cast<std::int32_t>(be_toint(vm, 2)));
    be_return_nil(vm);
}

/** @brief alarm(dB) arms the alarm; alarm(nil) or alarm() disarms it. */
int LevelAlarm(bvm* vm) {
    if (be_top(vm) >= 2 && be_isnumber(vm, 2))
        audio::level_set_alarm(true, static_cast<std::int32_t>(be_toint(vm, 2)));
    else
        audio::level_set_alarm(false, 0);
    be_return_nil(vm);
}

/** @brief Levels are dB reals; they appear only after the first 125 ms window. */
int LevelStatus(bvm* vm) {
    const auto s = audio::level_snapshot();
    be_newobject(vm, "map");
    field(vm, "running", s.running);
    field(vm, "windows", s.windows);
    field(vm, "clipped", s.clipped);
    if (s.windows) {
        real_field(vm, "level", db(s.level));
        real_field(vm, "peak", db(s.peak));
        real_field(vm, "leq", db(s.leq));
        real_field(vm, "min", db(s.min));
        real_field(vm, "max", db(s.max));
        real_field(vm, "l10", db(s.l10));
        real_field(vm, "l50", db(s.l50));
        real_field(vm, "l90", db(s.l90));
    }
    real_field(vm, "offset", s.offset_db);
    field(vm, "alarm_enabled", s.alarm_enabled);
    real_field(vm, "alarm_db", s.alarm_db);
    field(vm, "alarm_active", s.alarm_active);
    field(vm, "alarm_count", s.alarm_count);
    be_pop(vm, 1);
    be_return(vm);
}

/** @brief Returns {from, step, counts}: 5 dB buckets of 125 ms windows starting at `from` dB. */
int LevelHistogram(bvm* vm) {
    std::array<std::uint32_t, audio::kLevelHistogramBuckets> counts{};
    const auto from = audio::level_histogram(counts);
    be_newobject(vm, "map");
    real_field(vm, "from", from);
    real_field(vm, "step", audio::kLevelHistogramStepDb);
    be_pushstring(vm, "counts");
    be_newobject(vm, "list");
    for (const auto count : counts) {
        be_pushint(vm, static_cast<bint>(count));
        be_data_push(vm, -2);
        be_pop(vm, 1);
    }
    end_list_field(vm);
    be_pop(vm, 1);
    be_return(vm);
}

const bnfuncinfo level_methods[] = {{"start", LevelStart}, {"stop", LevelStop},
    {"reset", LevelReset}, {"offset", LevelOffset}, {"alarm", LevelAlarm}, {"status", LevelStatus},
    {"histogram", LevelHistogram}, {nullptr, nullptr}};

int SpectrumStart(bvm* vm) {
    checked(vm, audio::spectrum_start());
    be_return_nil(vm);
}

int SpectrumStop(bvm* vm) {
    checked(vm, audio::spectrum_stop());
    be_return_nil(vm);
}

int SpectrumReset(bvm* vm) {
    audio::spectrum_reset();
    be_return_nil(vm);
}

/** @brief The peak tone is `peak_hz` and `peak_db`; `peak_hz` is 0 on silence. */
int SpectrumStatus(bvm* vm) {
    const auto s = audio::spectrum_snapshot();
    be_newobject(vm, "map");
    field(vm, "running", s.running);
    field(vm, "frames", s.frames);
    field(vm, "fft_size", s.fft_size);
    field(vm, "bands", s.bands);
    field(vm, "peak_hz", s.peak_hz);
    real_field(vm, "peak_db", db(s.peak_level));
    be_pop(vm, 1);
    be_return(vm);
}

/** @brief bands(series) with series "latest" (default), "avg" or "max"; returns {hz, db} lists. */
int SpectrumBands(bvm* vm) {
    auto series = audio::SpectrumSeries::latest;
    if (be_top(vm) >= 2 && be_isstring(vm, 2)) {
        const std::string_view name = be_tostring(vm, 2);
        if (name == "avg")
            series = audio::SpectrumSeries::average;
        else if (name == "max")
            series = audio::SpectrumSeries::hold;
        else if (name != "latest")
            be_raise(vm, "value_error", "series is latest, avg or max");
    }
    std::array<std::uint32_t, audio::kSpectrumMaximumBands> centers{};
    std::array<std::int32_t, audio::kSpectrumMaximumBands> levels{};
    const auto count = audio::spectrum_series(series, centers, levels);
    be_newobject(vm, "map");
    be_pushstring(vm, "hz");
    be_newobject(vm, "list");
    for (std::size_t i = 0; i < count; ++i) {
        be_pushint(vm, static_cast<bint>(centers[i]));
        be_data_push(vm, -2);
        be_pop(vm, 1);
    }
    end_list_field(vm);
    be_pushstring(vm, "db");
    be_newobject(vm, "list");
    for (std::size_t i = 0; i < count; ++i) {
        be_pushreal(vm, static_cast<breal>(db(levels[i])));
        be_data_push(vm, -2);
        be_pop(vm, 1);
    }
    end_list_field(vm);
    be_pop(vm, 1);
    be_return(vm);
}

const bnfuncinfo spectrum_methods[] = {{"start", SpectrumStart}, {"stop", SpectrumStop},
    {"reset", SpectrumReset}, {"status", SpectrumStatus}, {"bands", SpectrumBands},
    {nullptr, nullptr}};

const bnfuncinfo methods[] = {{".id", nullptr}, {"init", Init}, {"start", Start}, {"stop", Stop},
    {"configure", Configure}, {"close", Close}, {"deinit", Deinit}, {"status", Status},
    {"tone", Tone}, {nullptr, nullptr}};
} // namespace

namespace {
/** @brief Builds a hidden global class, and optionally its singleton instance in its place. */
void install_class(bvm* vm, const char* name, const bnfuncinfo* table, bool singleton) {
    be_pushclass(vm, name, table);
    if (singleton)
        be_call(vm, 0); // The instance replaces the class on the stack.
    be_setglobal(vm, name);
    be_pop(vm, 1);
}

/** @brief Resolves `audio.Session`, `audio.level` and `audio.spectrum` for `import audio`. */
int audio_member(bvm* vm) {
    if (!be_isstring(vm, 1))
        be_raise(vm, "type_error", "audio member expects a name");
    const std::string_view name = be_tostring(vm, 1);
    const char* hidden = name == "Session" ? "_audio_session" : nullptr;
    if (name == "level")
        hidden = "_audio_level";
    if (name == "spectrum")
        hidden = "_audio_spectrum";
    if (hidden && be_getglobal(vm, hidden))
        be_return(vm);
    be_getmodule(vm, "undefined");
    be_return(vm);
}
} // namespace

namespace newmota {
/** @brief The classes stay out of the script namespace; `import audio` is the only way in. */
bool install_audio_module(bvm* vm) {
    install_class(vm, "_audio_session", methods, false);
    install_class(vm, "_audio_level", level_methods, true);
    install_class(vm, "_audio_spectrum", spectrum_methods, true);
    return true;
}

void remove_audio_module() {
    for (auto& s : instances)
        if (s.id && audio::stop(s.handle) == audio::Result::success)
            s = {};
}
} // namespace newmota

extern "C" {
be_native_module_attr_table(audio){be_native_module_function("member", audio_member)};
extern const bntvmodule_t be_native_module_audio = {
    "audio", audio_attrs, sizeof(audio_attrs) / sizeof(audio_attrs[0]), nullptr};
}
