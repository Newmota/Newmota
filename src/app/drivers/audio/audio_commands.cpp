/*
  audio_commands.cpp - Native audio command dispatch

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_commands.cpp
 * @brief Retains Tasmota codec indexes and bounded file-loop semantics.
 */
#include "drivers/audio/audio_commands.h"
#include "drivers/audio/audio_session.h"
#include "drivers/audio/i2s_service.h"
#include "newmota_features.h"
#include "drivers/audio/audio_store.h"
#include "newmota/platform/platform.h"
#include "services/time/time_service.h"
#include "support/number.h"
#include "support/text.h"
#include <algorithm>

namespace newmota::commands {
namespace {
/** @brief Preserves the public codec selector values accepted by Tasmota commands. */
enum class TasmotaCodecIndex : std::uint32_t { aac = 0, mp3 = 1, opus = 2 };

constexpr TasmotaCodecIndex kDefaultPlaybackCodec = TasmotaCodecIndex::aac;
constexpr TasmotaCodecIndex kDefaultRecordingCodec = TasmotaCodecIndex::mp3;

bool parse_codec_index(std::uint32_t raw, TasmotaCodecIndex& codec) {
    switch (raw) {
    case static_cast<std::uint32_t>(TasmotaCodecIndex::aac):
        codec = TasmotaCodecIndex::aac;
        return true;
    case static_cast<std::uint32_t>(TasmotaCodecIndex::mp3):
        codec = TasmotaCodecIndex::mp3;
        return true;
    case static_cast<std::uint32_t>(TasmotaCodecIndex::opus):
        codec = TasmotaCodecIndex::opus;
        return true;
    default:
        return false;
    }
}

bool audio_codec(TasmotaCodecIndex selected, bool recording, audio::Codec& codec) {
    switch (selected) {
    case TasmotaCodecIndex::aac:
        if (recording)
            return false;
        codec = audio::Codec::aac;
        return true;
    case TasmotaCodecIndex::mp3:
        codec = audio::Codec::mp3;
        return true;
    case TasmotaCodecIndex::opus:
        codec = audio::Codec::opus;
        return true;
    }
    return false;
}

CommandStatus response(
    CommandOutput& output, const char* name, audio::Result r, const char* accepted) {
    (void)output.format(
        "{\"%s\":\"%s\"}", name, r == audio::Result::success ? accepted : audio::result_name(r));
    return r == audio::Result::success && !output.failed() ? CommandStatus::handled
                                                           : CommandStatus::failed;
}

using WireConfig = platform::i2s::Config;

/** @brief One direction being edited: its wire profile and its gain. */
struct StagedProfile {
    WireConfig wire{};
    /** @brief Transmit: volume percent. Receive: microphone gain in Q12.4. */
    std::uint16_t gain{};
};

/** @brief Parses a non-negative decimal such as `30` or `2.5` into Q12.4, rounded to 1/16. */
bool parse_q12_4(std::string_view value, std::uint16_t& gain) {
    constexpr std::size_t kMaximumFractionDigits = 4;
    constexpr std::uint32_t kFractionScale = 10000;
    const auto dot = value.find('.');
    std::uint32_t whole{}, fraction{};
    const auto digits = dot == std::string_view::npos ? 0U : value.size() - dot - 1U;
    if (!support::parse_unsigned_decimal(value.substr(0, dot), kMaximumInputGain, whole) ||
        (dot != std::string_view::npos &&
            (!digits || digits > kMaximumFractionDigits ||
                !support::parse_unsigned_decimal(value.substr(dot + 1), kFractionScale, fraction))))
        return false;
    for (auto d = digits; d < kMaximumFractionDigits; ++d)
        fraction *= 10U;
    const auto q = whole * kUnityInputGain +
                   (fraction * kUnityInputGain + kFractionScale / 2U) / kFractionScale;
    if (q > kMaximumInputGain)
        return false;
    gain = static_cast<std::uint16_t>(q);
    return true;
}

/** @brief Applies one named field to a staged profile; false for an unknown name or bad value.
 * Tasmota's spellings are accepted as aliases: `SampleRate`, `SlotMask`, `SlotWidth`,
 * `DMAFrame`, `DMADesc`, `Gain`, `SlotConfig` (0 MSB, 1 PCM, 2 Philips), `BitShift`, `WsWidth`,
 * `WsPol`, `LeftAlign`, `BigEndian`, `LsbOrder`, `MclkInv`, `BclkInv`, `WsInv` and `APLL`.
 * Its `Sys` section, `Mode` numbers, `Channels` and filter keys are rejected by name.
 */
bool apply_field(
    StagedProfile& staged, bool transmit, std::string_view field, std::string_view value) {
    namespace wire = platform::i2s;
    auto& config = staged.wire;
    auto& tuning = config.tuning;
    const auto is = [field](std::string_view name, std::string_view alias = {}) {
        return support::ascii_iequals(field, name) ||
               (!alias.empty() && support::ascii_iequals(field, alias));
    };
    const auto text = [value](std::string_view option) {
        return support::ascii_iequals(value, option);
    };
    if (is("mode")) {
        if (text("standard"))
            config.mode = wire::Mode::standard;
        else if (text("tdm"))
            config.mode = wire::Mode::tdm;
        else if (text("pdm"))
            config.mode = wire::Mode::pdm;
        else
            return false;
    } else if (is("role")) {
        if (text("controller"))
            config.role = wire::Role::controller;
        else if (text("target"))
            config.role = wire::Role::target;
        else
            return false;
    } else if (is("msb_right")) {
        if (text("automatic"))
            config.msb_right = wire::MsbRight::automatic;
        else if (text("enabled"))
            config.msb_right = wire::MsbRight::enabled;
        else if (text("disabled"))
            config.msb_right = wire::MsbRight::disabled;
        else
            return false;
    } else if (is("pdm_format")) {
        if (text("pcm"))
            config.pdm_pcm = true;
        else if (text("raw"))
            config.pdm_pcm = false;
        else
            return false;
    } else if (is("framing")) {
        if (text("philips"))
            config.framing = wire::Framing::philips;
        else if (text("left"))
            config.framing = wire::Framing::left_justified;
        else if (text("right"))
            config.framing = wire::Framing::right_justified;
        else if (text("pcm"))
            config.framing = wire::Framing::pcm_short;
        else
            return false;
    } else if (is("gain")) {
        if (!transmit)
            return parse_q12_4(value, staged.gain);
        std::uint32_t percent{};
        if (!support::parse_unsigned_decimal(value, kMaximumOutputGainPercent, percent))
            return false;
        staged.gain = static_cast<std::uint16_t>(percent);
    } else {
        std::uint32_t number{};
        if (!support::parse_unsigned_decimal(value, wire::kMaximumRawPdmRateHz, number))
            return false;
        if (is("rate", "samplerate") && number)
            config.sample_rate = number;
        else if (is("slot_mask", "slotmask") && number && number < (1U << config.slots))
            config.slot_mask = number;
        else if (is("dma_frames", "dmaframe") && number && number <= wire::kMaximumDmaFrames)
            config.dma_frames = static_cast<std::uint16_t>(number);
        else if (is("pdm_clock") && number < PinId::kInvalid)
            config.pdm_clock = PinId{static_cast<std::uint16_t>(number)};
        else if (is("bits") && (number == 16 || number == 24 || number == 32)) {
            config.sample_bits = static_cast<wire::SampleWidth>(number);
            // A slot is never narrower than its sample; widening keeps single-field edits valid.
            if (wire::sample_width_bits(config.slot_bits) < number)
                config.slot_bits = config.sample_bits;
        } else if (is("slot_bits", "slotwidth") && (number == 16 || number == 24 || number == 32))
            config.slot_bits = static_cast<wire::SampleWidth>(number);
        else if (is("slotconfig") && number <= 2) // Tasmota: 0 MSB, 1 PCM, 2 Philips
            config.framing = number == 0   ? wire::Framing::left_justified
                             : number == 1 ? wire::Framing::pcm_short
                                           : wire::Framing::philips;
        else if (is("bitshift") && number <= 1)
            config.framing = number ? wire::Framing::philips : wire::Framing::left_justified;
        else if (is("ws_width", "wswidth") && number <= UINT8_MAX)
            tuning.ws_width = static_cast<std::uint8_t>(number);
        else if (is("dma_desc", "dmadesc") && number >= wire::kMinimumDmaDescriptors &&
                 number <= wire::kMaximumDmaDescriptors)
            tuning.dma_descriptors = static_cast<std::uint8_t>(number);
        else if (number <= 1) {
            const bool on = number != 0;
            if (is("ws_pol", "wspol"))
                tuning.ws_pol = on;
            else if (is("left_align", "leftalign"))
                tuning.left_align = on;
            else if (is("big_endian", "bigendian"))
                tuning.big_endian = on;
            else if (is("lsb_first", "lsborder"))
                tuning.lsb_first = on;
            else if (is("mclk_inv", "mclkinv"))
                tuning.mclk_inv = on;
            else if (is("bclk_inv", "bclkinv"))
                tuning.bclk_inv = on;
            else if (is("ws_inv", "wsinv"))
                tuning.ws_inv = on;
            else if (is("apll"))
                tuning.apll = on;
            else
                return false;
        } else
            return false;
    }
    return true;
}

/** @brief Checks the staged profile against the portable limits and this SoC's capabilities. */
audio::Result check_profile(const WireConfig& c, platform::i2s::Direction direction) {
    namespace wire = platform::i2s;
    if (wire::sample_width_bits(c.sample_bits) > wire::sample_width_bits(c.slot_bits) ||
        c.sample_rate > (c.mode == wire::Mode::pdm && !c.pdm_pcm ? wire::kMaximumRawPdmRateHz
                                                                 : wire::kMaximumPcmRateHz))
        return audio::Result::invalid_argument;
    const auto caps = wire::capabilities();
    const bool receive = direction == wire::Direction::receive;
    const wire::Tuning defaults{};
    if (!(caps.modes & wire::mode_mask(c.mode)) ||
        (c.mode == wire::Mode::pdm && !(receive ? caps.pdm_rx : caps.pdm_tx)) ||
        (!caps.tuning && !(c.tuning == defaults)) || (c.tuning.apll && !caps.apll) ||
        (!caps.serial_order && (c.tuning.left_align != defaults.left_align || c.tuning.big_endian ||
                                   c.tuning.lsb_first)))
        return audio::Result::unsupported;
    return audio::Result::success;
}

std::uint8_t tuning_flags(const platform::i2s::Tuning& t) {
    return static_cast<std::uint8_t>(
        (t.ws_pol ? kTuningWsPol : 0U) | (t.left_align ? kTuningLeftAlign : 0U) |
        (t.big_endian ? kTuningBigEndian : 0U) | (t.lsb_first ? kTuningLsbFirst : 0U) |
        (t.mclk_inv ? kTuningMclkInv : 0U) | (t.bclk_inv ? kTuningBclkInv : 0U) |
        (t.ws_inv ? kTuningWsInv : 0U) | (t.apll ? kTuningApll : 0U));
}

I2sProfileSettings capture_profile(const StagedProfile& staged) {
    const auto& c = staged.wire;
    I2sProfileSettings p;
    p.present = true;
    p.mode = static_cast<std::uint8_t>(c.mode);
    p.role = static_cast<std::uint8_t>(c.role);
    p.msb_right = static_cast<std::uint8_t>(c.msb_right);
    p.sample_bits = platform::i2s::sample_width_bits(c.sample_bits);
    p.slot_bits = platform::i2s::sample_width_bits(c.slot_bits);
    p.pdm_pcm = c.pdm_pcm;
    p.sample_rate = c.sample_rate;
    p.slot_mask = static_cast<std::uint16_t>(c.slot_mask);
    p.dma_frames = c.dma_frames;
    p.pdm_clock = c.pdm_clock.value();
    p.framing = static_cast<std::uint8_t>(c.framing);
    p.gain = staged.gain;
    p.ws_width = c.tuning.ws_width;
    p.tuning_flags = tuning_flags(c.tuning);
    p.dma_descriptors = c.tuning.dma_descriptors;
    return p;
}

/** @brief Overlays a stored profile on a config that already carries the template pins. */
void restore_profile(const I2sProfileSettings& p, WireConfig& c) {
    if (!p.present)
        return;
    c.mode = static_cast<platform::i2s::Mode>(p.mode);
    c.role = static_cast<platform::i2s::Role>(p.role);
    c.msb_right = static_cast<platform::i2s::MsbRight>(p.msb_right);
    c.sample_bits = static_cast<platform::i2s::SampleWidth>(p.sample_bits);
    c.slot_bits = static_cast<platform::i2s::SampleWidth>(p.slot_bits);
    c.pdm_pcm = p.pdm_pcm;
    c.sample_rate = p.sample_rate;
    c.slot_mask = p.slot_mask;
    c.dma_frames = p.dma_frames;
    c.pdm_clock = PinId{p.pdm_clock};
    c.framing = static_cast<platform::i2s::Framing>(p.framing);
    auto& t = c.tuning;
    t.ws_width = p.ws_width;
    t.ws_pol = p.tuning_flags & kTuningWsPol;
    t.left_align = p.tuning_flags & kTuningLeftAlign;
    t.big_endian = p.tuning_flags & kTuningBigEndian;
    t.lsb_first = p.tuning_flags & kTuningLsbFirst;
    t.mclk_inv = p.tuning_flags & kTuningMclkInv;
    t.bclk_inv = p.tuning_flags & kTuningBclkInv;
    t.ws_inv = p.tuning_flags & kTuningWsInv;
    t.apll = p.tuning_flags & kTuningApll;
    t.dma_descriptors = p.dma_descriptors;
}

/** @brief Minimal allocation-free scanner for `{"Tx":{...},"Rx":{...}}`.
 * An outer `"I2SConfig"` key is accepted so Tasmota-style payloads work. Leaves are strings
 * without escapes or unsigned decimal numbers, optionally with a fraction (Rx `Gain`).
 */
class ProfileObject {
  public:
    explicit ProfileObject(std::string_view text) : text_(text) {}

    /** @brief Calls `visit(section, key, value)` per leaf; false on bad syntax or a rejection. */
    template <typename Visit> bool parse(Visit&& visit) {
        return sections(false, visit) && (whitespace(), position_ == text_.size());
    }

  private:
    std::string_view text_;
    std::size_t position_{};

    void whitespace() {
        while (
            position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\t' ||
                                            text_[position_] == '\r' || text_[position_] == '\n'))
            ++position_;
    }

    bool take(char token) {
        whitespace();
        if (position_ == text_.size() || text_[position_] != token)
            return false;
        ++position_;
        return true;
    }

    bool string(std::string_view& output) {
        if (!take('"'))
            return false;
        const auto start = position_;
        while (position_ < text_.size() && text_[position_] != '"') {
            const auto c = static_cast<unsigned char>(text_[position_++]);
            if (c < 0x20 || c == '\\')
                return false;
        }
        output = text_.substr(start, position_ - start);
        return take('"');
    }

    bool scalar(std::string_view& output) {
        whitespace();
        if (position_ < text_.size() && text_[position_] == '"')
            return string(output);
        const auto start = position_;
        while (position_ < text_.size() &&
               ((text_[position_] >= '0' && text_[position_] <= '9') || text_[position_] == '.'))
            ++position_;
        output = text_.substr(start, position_ - start);
        return !output.empty();
    }

    template <typename Visit> bool leaves(std::string_view section, Visit& visit) {
        if (!take('{'))
            return false;
        if (take('}'))
            return true;
        do {
            std::string_view key, value;
            if (!string(key) || !take(':') || !scalar(value) || !visit(section, key, value))
                return false;
        } while (take(','));
        return take('}');
    }

    template <typename Visit> bool sections(bool wrapped, Visit& visit) {
        if (!take('{'))
            return false;
        if (take('}'))
            return true;
        do {
            std::string_view key;
            if (!string(key) || !take(':'))
                return false;
            if (!wrapped && support::ascii_iequals(key, "I2SConfig")) {
                if (!sections(true, visit))
                    return false;
            } else if (!leaves(key, visit))
                return false;
        } while (take(','));
        return take('}');
    }
};

/** @brief True when a rejected name is short and plain enough to echo in a response. */
bool printable_name(std::string_view name) {
    constexpr std::size_t kMaximumNameBytes = 24;
    if (name.empty() || name.size() > kMaximumNameBytes)
        return false;
    for (const auto c : name)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                c == '_'))
            return false;
    return true;
}

/** @brief Sets profile fields atomically and persists them in the audio settings file.
 * Forms: `Rx.rate 16000`, `{"Rx":{"rate":16000,"slot_mask":1}}`, `reset`, or a query. Every
 * field is staged and validated before anything is saved, and the runtime changes only after
 * the save succeeds, so a rejected command leaves both untouched. Pins are never stored.
 */
CommandStatus configure(const CommandRequest& request, CommandOutput& output) {
    if (request.has_index)
        return CommandStatus::invalid;
    auto& defaults = audio::command_config();
    const auto payload = support::trim_ascii(request.payload);
    if (!payload.empty() && payload != "?") {
        if (audio::command_status().state == audio::State::running)
            return response(output, "I2SConfig", audio::Result::busy, "");
        auto stored = audio::stored_settings();
        // The live gains are what a save stores, as Tasmota keeps I2SGain in its settings.
        StagedProfile tx{defaults.output, audio::output_gain()};
        StagedProfile rx{defaults.input, audio::input_gain()};
        bool tx_touched{}, rx_touched{};
        if (support::ascii_iequals(payload, "reset")) {
            if (!audio::store_settings({}))
                return CommandStatus::failed;
            (void)i2s::configured_config(defaults.input, platform::i2s::Direction::receive);
            (void)i2s::configured_config(defaults.output, platform::i2s::Direction::transmit);
            audio::set_output_gain(kDefaultOutputGainPercent);
            audio::set_input_gain(kUnityInputGain);
            return output.write("{\"I2SConfig\":\"Reset\"}") ? CommandStatus::handled
                                                             : CommandStatus::failed;
        }
        const auto stage = [&](std::string_view section, std::string_view field,
                               std::string_view value) {
            const bool is_tx = support::ascii_iequals(section, "tx");
            if (!is_tx && !support::ascii_iequals(section, "rx"))
                return false;
            (is_tx ? tx_touched : rx_touched) = true;
            return apply_field(is_tx ? tx : rx, is_tx, field, value);
        };
        std::string_view bad_section, bad_key;
        if (payload.front() == '{') {
            if (!ProfileObject{payload}.parse(
                    [&](std::string_view section, std::string_view key, std::string_view value) {
                        if (stage(section, key, value))
                            return true;
                        bad_section = section;
                        bad_key = key;
                        return false;
                    })) {
                if (printable_name(bad_section) && printable_name(bad_key)) {
                    (void)output.format("{\"I2SConfig\":\"Invalid %.*s.%.*s\"}",
                        static_cast<int>(bad_section.size()), bad_section.data(),
                        static_cast<int>(bad_key.size()), bad_key.data());
                    return CommandStatus::failed;
                }
                return CommandStatus::invalid;
            }
        } else {
            const auto separator = payload.find(' ');
            const auto dot = payload.find('.');
            if (separator == std::string_view::npos || dot == std::string_view::npos ||
                dot >= separator ||
                !stage(payload.substr(0, dot), payload.substr(dot + 1, separator - dot - 1),
                    support::trim_ascii(payload.substr(separator + 1))))
                return CommandStatus::invalid;
        }
        if (tx_touched) {
            const auto checked = check_profile(tx.wire, platform::i2s::Direction::transmit);
            if (checked != audio::Result::success)
                return response(output, "I2SConfig", checked, "");
            stored.transmit = capture_profile(tx);
        }
        if (rx_touched) {
            const auto checked = check_profile(rx.wire, platform::i2s::Direction::receive);
            if (checked != audio::Result::success)
                return response(output, "I2SConfig", checked, "");
            stored.receive = capture_profile(rx);
        }
        if (!audio::store_settings(stored))
            return CommandStatus::failed;
        defaults.output = tx.wire;
        defaults.input = rx.wire;
        audio::set_output_gain(static_cast<std::uint8_t>(tx.gain));
        audio::set_input_gain(rx.gain);
    }
    const auto mode = [](platform::i2s::Mode selected) {
        switch (selected) {
        case platform::i2s::Mode::standard:
            return "standard";
        case platform::i2s::Mode::tdm:
            return "tdm";
        case platform::i2s::Mode::pdm:
            return "pdm";
        }
        return "invalid";
    };
    const auto framing = [](platform::i2s::Framing selected) {
        switch (selected) {
        case platform::i2s::Framing::philips:
            return "philips";
        case platform::i2s::Framing::left_justified:
            return "left";
        case platform::i2s::Framing::right_justified:
            return "right";
        case platform::i2s::Framing::pcm_short:
            return "pcm";
        }
        return "invalid";
    };
    const auto profile = [&](const platform::i2s::Config& c, bool transmit) {
        const auto msb_right = c.msb_right == platform::i2s::MsbRight::automatic ? "automatic"
                               : c.msb_right == platform::i2s::MsbRight::enabled ? "enabled"
                                                                                 : "disabled";
        const auto& t = c.tuning;
        // Receive gain is Q12.4: whole part, then sixteenths as up to four decimals.
        const auto in_gain = audio::input_gain();
        const unsigned whole = transmit ? audio::output_gain() : in_gain / kUnityInputGain;
        unsigned fraction = transmit ? 0U : (in_gain % kUnityInputGain) * 625U;
        unsigned decimals = fraction ? 4U : 0U;
        for (; decimals && fraction % 10U == 0U; --decimals)
            fraction /= 10U;
        return output.format("{\"mode\":\"%s\",\"role\":\"%s\",\"rate\":%lu,\"bits\":%u,"
                             "\"slot_bits\":%u,\"slot_mask\":%lu",
                   mode(c.mode),
                   c.role == platform::i2s::Role::controller ? "controller" : "target",
                   static_cast<unsigned long>(c.sample_rate), unsigned(c.sample_bits),
                   unsigned(c.slot_bits), static_cast<unsigned long>(c.slot_mask)) &&
               output.format(",\"dma_frames\":%u,\"msb_right\":\"%s\",\"pdm_format\":\"%s\",\"pdm_"
                             "clock\":%d",
                   unsigned(c.dma_frames), msb_right, c.pdm_pcm ? "pcm" : "raw",
                   platform::i2s::pdm_clock_pin(c).valid() ? platform::i2s::pdm_clock_pin(c).value()
                                                           : -1) &&
               output.format(",\"framing\":\"%s\",\"gain\":%u", framing(c.framing), whole) &&
               (!decimals || output.format(".%0*u", int(decimals), fraction)) &&
               output.format(",\"ws_width\":%u,\"ws_pol\":%u,\"left_align\":%u,\"big_endian\":%u,"
                             "\"lsb_first\":%u,\"mclk_inv\":%u,\"bclk_inv\":%u,\"ws_inv\":%u,"
                             "\"apll\":%u,\"dma_desc\":%u}",
                   unsigned(t.ws_width), unsigned(t.ws_pol), unsigned(t.left_align),
                   unsigned(t.big_endian), unsigned(t.lsb_first), unsigned(t.mclk_inv),
                   unsigned(t.bclk_inv), unsigned(t.ws_inv), unsigned(t.apll),
                   unsigned(t.dma_descriptors));
    };
    const auto& saved = audio::stored_settings();
    return output.write("{\"I2SConfig\":{\"Tx\":") && profile(defaults.output, true) &&
                   output.write(",\"Rx\":") && profile(defaults.input, false) &&
                   output.format(",\"Saved\":{\"Tx\":%u,\"Rx\":%u}}}",
                       unsigned(saved.transmit.present), unsigned(saved.receive.present))
               ? CommandStatus::handled
               : CommandStatus::failed;
}
#if USE_AUDIO_SAM
/** @brief Longest phrase is about 45 characters; the location buffer holds 320. */
constexpr std::size_t kSayTimeBytes = 64;

/** @brief Appends `word` and a space; the buffer is sized for the longest phrase. */
void append_word(std::array<char, kSayTimeBytes>& text, std::size_t& used, std::string_view word) {
    if (used + word.size() + 1U >= text.size())
        return;
    std::copy(word.begin(), word.end(), text.begin() + static_cast<std::ptrdiff_t>(used));
    used += word.size();
    text[used++] = ' ';
}

/** @brief Spells "the time is three forty five p m" for SAM, which reads words, not digits. */
std::size_t say_time_text(
    std::uint8_t hour, std::uint8_t minute, std::array<char, kSayTimeBytes>& text) {
    static constexpr std::array<std::string_view, 20> kBelowTwenty{"twelve", "one", "two", "three",
        "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve", "thirteen",
        "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
    static constexpr std::array<std::string_view, 6> kTens{
        "oh", "ten", "twenty", "thirty", "forty", "fifty"};
    std::size_t used{};
    for (const auto word : {"the", "time", "is"})
        append_word(text, used, word);
    append_word(text, used, kBelowTwenty[hour % 12U]);
    if (!minute)
        append_word(text, used, "o clock");
    else if (minute >= 11 && minute <= 19)
        append_word(text, used, kBelowTwenty[minute]);
    else {
        append_word(text, used, kTens[minute / 10U]);
        if (minute % 10U)
            append_word(text, used, kBelowTwenty[minute % 10U]);
    }
    // SAM reads single letters poorly; spell them as they sound.
    append_word(text, used, hour >= 12 ? "pee" : "ay");
    append_word(text, used, "em");
    text[used ? used - 1U : 0U] = '\0';
    return used ? used - 1U : 0U;
}
#endif
} // namespace

void apply_audio_settings(const AudioSettings& stored) {
    auto& defaults = audio::command_config();
    restore_profile(stored.receive, defaults.input);
    restore_profile(stored.transmit, defaults.output);
    audio::set_output_gain(static_cast<std::uint8_t>(
        stored.transmit.present ? stored.transmit.gain : kDefaultOutputGainPercent));
    audio::set_input_gain(stored.receive.present ? stored.receive.gain : kUnityInputGain);
}

CommandStatus dispatch_audio(const CommandRequest& request, CommandOutput& output) {
    if (support::ascii_iequals(request.name, "I2SConfig"))
        return configure(request, output);
    const bool wr = support::ascii_iequals(request.name, "I2SWr");
    const bool play = support::ascii_iequals(request.name, "I2SPlay");
    const bool record = support::ascii_iequals(request.name, "I2SRec");
    const bool loop = support::ascii_iequals(request.name, "I2SLoop");
    const bool stream = support::ascii_iequals(request.name, "I2SStream");
    const bool stop = support::ascii_iequals(request.name, "I2SStop");
    const bool gain = support::ascii_iequals(request.name, "I2SGain");
    const bool pause = support::ascii_iequals(request.name, "I2SPause");
    const bool say = support::ascii_iequals(request.name, "I2SSay");
    if (!wr && !play && !record && !loop && !stream && !stop && !gain && !pause && !say)
        return CommandStatus::unknown;
    const auto* name = wr       ? "I2SWr"
                       : play   ? "I2SPlay"
                       : record ? "I2SRec"
                       : loop   ? "I2SLoop"
                       : stop   ? "I2SStop"
                       : gain   ? "I2SGain"
                       : pause  ? "I2SPause"
                       : say    ? "I2SSay"
                                : "I2SStream";
    if (stop || gain || pause) {
        if (request.has_index)
            return CommandStatus::invalid;
        if (stop)
            return response(output, name, audio::stop_command(), "Done");
        if (pause)
            return response(output, name, audio::pause_command(true), "Paused");
        if (!request.is_query()) {
            std::uint32_t percent{};
            if (!support::parse_unsigned_decimal(
                    support::trim_ascii(request.payload), 100, percent))
                return CommandStatus::invalid;
            audio::set_output_gain(static_cast<std::uint8_t>(percent));
        }
        return output.format("{\"I2SGain\":%u}", unsigned(audio::output_gain()))
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    }
    if (stream) {
        if (request.has_index)
            return CommandStatus::invalid;
        if (!request.is_query()) {
            std::uint32_t enabled{};
            if (!support::parse_unsigned_decimal(support::trim_ascii(request.payload), 1, enabled))
                return CommandStatus::invalid;
            const auto r = audio::stream_control(enabled != 0);
            if (r != audio::Result::success)
                return response(output, name, r, "");
        }
        return output.format("{\"I2SStream\":%u}", unsigned(audio::stream_enabled()))
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    }
    const auto payload = support::trim_ascii(request.payload);
    if (record && payload == "-?") {
        return output.format("{\"I2SREC-duration\":%lu}",
                   static_cast<unsigned long>(audio::command_status().elapsed_seconds))
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    }
    if (payload == "?") {
        const auto status = audio::command_status();
        return output.format(
                   "{\"%s\":{\"State\":%u,\"InputBytes\":%lu,\"OutputBytes\":%lu,\"Loops\":%lu}}",
                   name, unsigned(status.state), static_cast<unsigned long>(status.input_bytes),
                   static_cast<unsigned long>(status.output_bytes),
                   static_cast<unsigned long>(status.loops))
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    }
    if (payload.empty()) {
        if (record)
            return response(output, name, audio::finish_command(), "Stopping");
        if (wr)
            return response(output, name, audio::stop_command(), "Stopped");
        // A bare I2SPlay or I2SLoop resumes a paused file, as in Tasmota.
        return response(output, name, audio::pause_command(false), "Resumed");
    }
    auto config = audio::command_config();
    if (payload.size() >= config.location.size() || payload.find('\0') != std::string_view::npos)
        return CommandStatus::invalid;
    config.location.fill(0);
    std::copy(payload.begin(), payload.end(), config.location.begin());
    config.source = record ? audio::Source::microphone
                    : wr   ? audio::Source::network
                    : say  ? audio::Source::text
                           : audio::Source::file;
    config.sink = record ? audio::Sink::file : audio::Sink::output;
    config.loop = loop;
    if (say) {
        // SAM renders the text before returning, so this command blocks for that time.
        if (request.has_index)
            return CommandStatus::invalid;
        config.codec = audio::Codec::sam;
        return response(output, name, audio::start_command(config), "Started");
    }
    auto selected_codec = record ? kDefaultRecordingCodec : kDefaultPlaybackCodec;
    if (request.has_index && !parse_codec_index(request.index, selected_codec))
        return CommandStatus::invalid;
    if (!audio_codec(selected_codec, record, config.codec))
        return CommandStatus::invalid;
    return response(output, name, audio::start_command(config), "Started");
}

CommandStatus dispatch_audio_say_time(
    const CommandRequest& request, const TimeService& time, CommandOutput& output) {
    if (!support::ascii_iequals(request.name, "I2STime"))
        return CommandStatus::unknown;
#if USE_AUDIO_SAM
    if (request.has_index)
        return CommandStatus::invalid;
    const auto snapshot = time.snapshot(platform::monotonic_milliseconds());
    if (!snapshot.valid) {
        (void)output.write("{\"I2STime\":\"Time not set\"}");
        return CommandStatus::failed;
    }
    const auto civil = TimeService::civil_time(snapshot.local_seconds);
    auto config = audio::command_config();
    config.location.fill(0);
    std::array<char, kSayTimeBytes> text{};
    say_time_text(civil.hour, civil.minute, text);
    std::copy(text.begin(), text.end(), config.location.begin());
    config.source = audio::Source::text;
    config.sink = audio::Sink::output;
    config.codec = audio::Codec::sam;
    config.loop = false;
    return response(output, "I2STime", audio::start_command(config), "Started");
#else
    (void)time;
    (void)output;
    return CommandStatus::unknown;
#endif
}
} // namespace newmota::commands
