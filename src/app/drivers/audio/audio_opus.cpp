/*
  audio_opus.cpp - Carried fixed-point Opus encoder and bounded WebM/Ogg support

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_opus.cpp
 * @brief Retains Tasmota Opus tuning with bounded packet and file framing.
 */
#include "drivers/audio/audio_codecs.h"
#include "newmota_features.h"

#if USE_AUDIO_OPUS
#include "libopus/opus.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <limits>
#include <new>
#include <string_view>

namespace newmota::audio::detail {
namespace {
constexpr std::size_t kMaximumOpusPacket = 8192;
constexpr std::size_t kMaximumEncodedPacket = 1296;
constexpr std::size_t kMaximumFrameSamples = 5760;
constexpr std::uint32_t kWebmClusterMilliseconds = 1000;
constexpr std::uint32_t kOpusClockRateHz = 48000;
constexpr std::uint32_t kOpusVoiceRateLimitHz = 16000;
constexpr std::uint32_t kOpusPacketDurationMilliseconds = 20;
constexpr std::uint32_t kMillisecondsPerSecond = 1000;
constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000U;
constexpr opus_int32 kMinimumMuxedOpusPacketBytes = 3;
constexpr int kTasmotaEncoderComplexity = 1;
constexpr std::array<std::uint32_t, 5> kSupportedEncoderRatesHz{
    8000, 12000, 16000, 24000, kOpusClockRateHz};
constexpr std::size_t kMaximumWebmDepth = 4;
constexpr std::size_t kWebmIdBytes = 4;
constexpr std::size_t kWebmSizeBytes = 8;
constexpr std::size_t kWebmBlockTimecodeBytes = 2;
constexpr std::size_t kWebmBlockFlagsBytes = 1;
constexpr std::uint8_t kMaximumWebmTrackNumberVintBytes = 4;
constexpr std::size_t kMinimumWebmBlockHeaderBytes =
    sizeof(std::uint8_t) + kWebmBlockTimecodeBytes + kWebmBlockFlagsBytes;
constexpr std::size_t kMaximumWebmBlockHeaderBytes =
    kMaximumWebmTrackNumberVintBytes + kWebmBlockTimecodeBytes + kWebmBlockFlagsBytes;
constexpr std::size_t kMaximumWebmCodecIdBytes = 16;
constexpr std::size_t kMinimumOpusBlockPayloadBytes = 1;
constexpr std::size_t kMaximumChannels = platform::i2s::kStereoChannelCount;
constexpr std::uint8_t kOpusHeadVersion = 1;
constexpr std::size_t kOpusHeadVersionOffset = 8;
constexpr std::size_t kOpusHeadChannelsOffset = 9;
constexpr std::size_t kOpusHeadPreSkipOffset = 10;
constexpr std::size_t kOpusHeadSampleRateOffset = 12;
constexpr std::size_t kOpusHeadMappingFamilyOffset = 18;
constexpr std::size_t kOpusHeadBytes = 19;
constexpr std::size_t kWebmTimestampScaleWidthBytes = 3;
constexpr std::uint64_t kWebmTimestampScaleNanoseconds = 1'000'000;
constexpr std::uint64_t kWebmOpusSeekPreRollNanoseconds = 80'000'000;
constexpr std::uint64_t kEbmlVersion = 1;
constexpr std::uint64_t kWebmDocumentVersion = 4;
constexpr std::uint64_t kWebmDocumentReadVersion = 2;
constexpr std::uint64_t kWebmFirstTrackNumber = 1;
constexpr std::uint64_t kWebmFirstTrackUid = 1;
constexpr std::uint8_t kMaximumCompatibleOpusHeadVersion = 15;
constexpr std::uint8_t kNoOpusChannelMapping = 0;
constexpr std::uint32_t kOggOpusHeadPacketIndex = 0;
constexpr std::uint32_t kOggOpusTagsPacketIndex = 1;
constexpr std::uint8_t kOggContinuedPacketLace = 255;
constexpr std::uint8_t kOggPageEndOfStreamFlag = 0x04U;
constexpr std::uint8_t kWebmBlockLacingFlags = 0x06U;
constexpr std::uint8_t kWebmFirstTrackNumberVint = 0x81U;
constexpr std::uint8_t kWebmBlockKeyframeFlag = 0x80U;
constexpr std::size_t kOggPageVersionOffset = 4;
constexpr std::size_t kOggPageHeaderTypeOffset = 5;
constexpr std::size_t kOggPageSegmentCountOffset = 26;
constexpr std::size_t kOggPageHeaderBytes = kOggPageSegmentCountOffset + sizeof(std::uint8_t);
constexpr std::size_t kOggMaximumSegments = 255;
constexpr std::size_t kWebmSniffBytes = kWebmIdBytes;
constexpr std::size_t kWebmHeaderChildrenBytes = 256;
constexpr std::size_t kWebmAudioChildrenBytes = 32;
constexpr std::size_t kWebmCodecPrivateBytes = 256;
constexpr std::size_t kWebmHeaderBufferBytes = 384;
constexpr std::size_t kOpusEncoderPcmSamples =
    kOpusClockRateHz * kOpusPacketDurationMilliseconds / kMillisecondsPerSecond * kMaximumChannels;
constexpr std::string_view kOpusHeadSignature = "OpusHead";
constexpr std::string_view kWebmOpusCodecId = "A_OPUS";

/** @brief Matroska element identifiers used by the bounded WebM reader/writer. */
enum class WebmElementId : std::uint32_t {
    ebml = 0x1A45DFA3,
    ebml_version = 0x4286,
    ebml_read_version = 0x42F7,
    ebml_max_id_length = 0x42F2,
    ebml_max_size_length = 0x42F3,
    doc_type = 0x4282,
    doc_type_version = 0x4287,
    doc_type_read_version = 0x4285,
    segment = 0x18538067,
    timestamp_scale = 0x2AD7B1,
    muxing_app = 0x4D80,
    writing_app = 0x5741,
    info = 0x1549A966,
    track_number = 0xD7,
    track_uid = 0x73C5,
    track_type = 0x83,
    codec_id = 0x86,
    codec_private = 0x63A2,
    codec_delay = 0x56AA,
    seek_pre_roll = 0x56BB,
    sampling_frequency = 0xB5,
    channels = 0x9F,
    audio = 0xE1,
    tracks = 0x1654AE6B,
    track_entry = 0xAE,
    cluster = 0x1F43B675,
    timecode = 0xE7,
    simple_block = 0xA3,
};

constexpr std::uint32_t webm_id(WebmElementId id) {
    return static_cast<std::uint32_t>(id);
}

constexpr std::string_view kOggCapturePattern = "OggS";
constexpr std::array<std::uint8_t, kWebmSniffBytes> kWebmEbmlSignature{
    static_cast<std::uint8_t>(webm_id(WebmElementId::ebml) >> 24U),
    static_cast<std::uint8_t>(webm_id(WebmElementId::ebml) >> 16U),
    static_cast<std::uint8_t>(webm_id(WebmElementId::ebml) >> 8U),
    static_cast<std::uint8_t>(webm_id(WebmElementId::ebml)),
};

constexpr std::uint64_t kWebmAudioTrackType = 2;

enum class Format : std::uint8_t { unknown, ogg, webm };

/** @brief Small bounded writer for the fixed WebM header and one audio block. */
struct Writer {
    std::uint8_t* bytes{};
    std::size_t capacity{}, size{};

    bool put(std::uint8_t value) {
        if (size == capacity)
            return false;
        bytes[size++] = value;
        return true;
    }

    bool append(const void* source, std::size_t length) {
        if (length > capacity - size)
            return false;
        std::memcpy(bytes + size, source, length);
        size += length;
        return true;
    }

    bool id(WebmElementId value) {
        auto numeric_id = webm_id(value);
        unsigned width = 1;
        for (auto remaining = numeric_id; remaining > std::numeric_limits<std::uint8_t>::max();
            remaining >>= 8U)
            ++width;
        for (unsigned i = width; i > 0; --i)
            if (!put(static_cast<std::uint8_t>(numeric_id >> ((i - 1U) * 8U))))
                return false;
        return true;
    }

    bool vint(std::uint64_t value) {
        unsigned width = 1;
        while (width < 8 && value >= ((std::uint64_t{1} << (7U * width)) - 1U))
            ++width;
        if (width == 8 && value >= ((std::uint64_t{1} << 56U) - 1U))
            return false;
        for (unsigned i = width; i > 0; --i) {
            auto byte = static_cast<std::uint8_t>(value >> ((i - 1U) * 8U));
            if (i == width)
                byte |= static_cast<std::uint8_t>(1U << (8U - width));
            if (!put(byte))
                return false;
        }
        return true;
    }

    bool unknown_vint() {
        if (!put(0x01U))
            return false;
        for (unsigned i = 1; i < 8; ++i)
            if (!put(0xFFU))
                return false;
        return true;
    }

    bool uint_element(WebmElementId element_id, std::uint64_t value, unsigned value_width = 0) {
        if (!value_width) {
            value_width = 1;
            while (value_width < 8 && value >= (std::uint64_t{1} << (8U * value_width)))
                ++value_width;
        }
        if (!id(element_id) || !vint(value_width))
            return false;
        for (unsigned i = value_width; i > 0; --i)
            if (!put(static_cast<std::uint8_t>(value >> ((i - 1U) * 8U))))
                return false;
        return true;
    }

    bool string_element(WebmElementId element_id, const char* text) {
        const auto length = std::strlen(text);
        return id(element_id) && vint(length) && append(text, length);
    }

    bool raw_element(WebmElementId element_id, const void* data, std::size_t length) {
        return id(element_id) && vint(length) && append(data, length);
    }

    bool master(WebmElementId element_id, const std::uint8_t* children, std::size_t length) {
        return id(element_id) && vint(length) && append(children, length);
    }
};

/** @brief Appends the canonical Matroska/WebM Opus header used by Tasmota. */
bool make_webm_header(std::uint8_t* out, std::size_t capacity, std::size_t& size,
    std::uint32_t sample_rate, std::uint8_t channels, std::uint16_t pre_skip,
    std::uint64_t codec_delay_ns) {
    std::array<std::uint8_t, kWebmHeaderChildrenBytes> children{};
    Writer ebml{children.data(), children.size()};
    if (!ebml.uint_element(WebmElementId::ebml_version, kEbmlVersion) ||
        !ebml.uint_element(WebmElementId::ebml_read_version, kEbmlVersion) ||
        !ebml.uint_element(WebmElementId::ebml_max_id_length, kWebmIdBytes) ||
        !ebml.uint_element(WebmElementId::ebml_max_size_length, kWebmSizeBytes) ||
        !ebml.string_element(WebmElementId::doc_type, "webm") ||
        !ebml.uint_element(WebmElementId::doc_type_version, kWebmDocumentVersion) ||
        !ebml.uint_element(WebmElementId::doc_type_read_version, kWebmDocumentReadVersion))
        return false;
    Writer result{out, capacity};
    if (!result.master(WebmElementId::ebml, children.data(), ebml.size) ||
        !result.id(WebmElementId::segment) || !result.unknown_vint())
        return false;

    children.fill(0);
    Writer info{children.data(), children.size()};
    if (!info.uint_element(WebmElementId::timestamp_scale, kWebmTimestampScaleNanoseconds,
            kWebmTimestampScaleWidthBytes) ||
        !info.string_element(WebmElementId::muxing_app, "Newmota") ||
        !info.string_element(WebmElementId::writing_app, "Newmota") ||
        !result.master(WebmElementId::info, children.data(), info.size))
        return false;

    children.fill(0);
    Writer track{children.data(), children.size()};
    if (!track.uint_element(WebmElementId::track_number, kWebmFirstTrackNumber) ||
        !track.uint_element(WebmElementId::track_uid, kWebmFirstTrackUid) ||
        !track.uint_element(WebmElementId::track_type, kWebmAudioTrackType) ||
        !track.string_element(WebmElementId::codec_id, kWebmOpusCodecId.data()))
        return false;
    std::array<std::uint8_t, kOpusHeadBytes> opus_head{};
    std::copy(kOpusHeadSignature.begin(), kOpusHeadSignature.end(), opus_head.begin());
    opus_head[kOpusHeadVersionOffset] = kOpusHeadVersion;
    opus_head[kOpusHeadChannelsOffset] = channels;
    opus_head[kOpusHeadPreSkipOffset] = static_cast<std::uint8_t>(pre_skip);
    opus_head[kOpusHeadPreSkipOffset + 1] = static_cast<std::uint8_t>(pre_skip >> 8U);
    for (std::size_t byte = 0; byte < sizeof(sample_rate); ++byte)
        opus_head[kOpusHeadSampleRateOffset + byte] =
            static_cast<std::uint8_t>(sample_rate >> (byte * 8U));
    if (!track.raw_element(WebmElementId::codec_private, opus_head.data(), opus_head.size()) ||
        !track.uint_element(WebmElementId::codec_delay, codec_delay_ns, sizeof(codec_delay_ns)) ||
        !track.uint_element(WebmElementId::seek_pre_roll, kWebmOpusSeekPreRollNanoseconds,
            sizeof(kWebmOpusSeekPreRollNanoseconds)))
        return false;

    std::array<std::uint8_t, kWebmAudioChildrenBytes> audio_children{};
    Writer audio{audio_children.data(), audio_children.size()};
    std::uint64_t sample_bits{};
    static_assert(sizeof(double) == sizeof(sample_bits));
    const double rate = static_cast<double>(sample_rate);
    std::memcpy(&sample_bits, &rate, sizeof(rate));
    if (!audio.id(WebmElementId::sampling_frequency) || !audio.vint(sizeof(rate)))
        return false;
    for (unsigned i = sizeof(rate); i > 0; --i)
        if (!audio.put(static_cast<std::uint8_t>(sample_bits >> ((i - 1U) * 8U))))
            return false;
    if (!audio.uint_element(WebmElementId::channels, channels) ||
        !track.master(WebmElementId::audio, audio_children.data(), audio.size) || !track.size ||
        !result.master(WebmElementId::tracks, children.data(), track.size))
        return false;
    size = result.size;
    return true;
}

/** @brief Converts one Opus packet into a Matroska SimpleBlock. */
bool make_cluster(std::uint8_t* out, std::size_t capacity, std::size_t& size,
    std::uint32_t cluster_ms, std::uint16_t relative_ms, const std::uint8_t* packet,
    std::size_t packet_size) {
    Writer writer{out, capacity};
    if (!writer.id(WebmElementId::cluster) || !writer.unknown_vint() ||
        !writer.uint_element(WebmElementId::timecode, cluster_ms, sizeof(cluster_ms)))
        return false;
    const auto payload_size = kMinimumWebmBlockHeaderBytes + packet_size;
    if (!writer.id(WebmElementId::simple_block) || !writer.vint(payload_size) ||
        !writer.put(kWebmFirstTrackNumberVint) ||
        !writer.put(static_cast<std::uint8_t>(relative_ms >> 8U)) ||
        !writer.put(static_cast<std::uint8_t>(relative_ms)) ||
        !writer.put(kWebmBlockKeyframeFlag) || !writer.append(packet, packet_size))
        return false;
    size = writer.size;
    return true;
}

struct OpusStage {
    Config config{};
    bool encode{}, header_written{}, format_ready{}, page_eos{}, eos_seen{}, packet_ready{};
    Format format{Format::unknown};
    OpusEncoder* encoder{};
    OpusDecoder* decoder{};
    std::uint32_t sample_rate{}, frame_samples{}, time_ms{}, cluster_ms{};
    std::uint8_t channels{};
    std::array<std::uint8_t, kWebmSniffBytes> sniff{};
    std::size_t sniff_size{};
    std::array<std::uint8_t, kOggPageHeaderBytes> ogg_header{};
    std::size_t ogg_header_size{};
    std::array<std::uint8_t, kOggMaximumSegments> ogg_lacing{};
    std::size_t ogg_lacing_count{}, ogg_lacing_size{}, ogg_segment{}, ogg_segment_left{};
    bool ogg_reading_lacing{}, ogg_page_eos{};
    std::uint32_t ogg_packet_index{};
    std::uint64_t webm_position{}, webm_element_start{}, webm_element_size{}, webm_remaining{};
    std::uint64_t webm_value{};
    std::uint32_t webm_element_id{};
    std::uint32_t pre_skip_left{};
    std::uint8_t webm_id_length{}, webm_id_expected{}, webm_size_length{}, webm_size_expected{};
    std::uint8_t webm_phase{}, webm_store_kind{};

    struct WebmMaster {
        std::uint64_t end{};
        std::uint32_t id{};
        bool unknown{};
    } webm_stack[kMaximumWebmDepth]{};

    std::size_t webm_depth{}, webm_store_size{};
    std::array<std::uint8_t, kMaximumOpusPacket + kMaximumWebmBlockHeaderBytes> webm_block{};
    std::array<std::uint8_t, kWebmCodecPrivateBytes> webm_codec_private{};
    std::array<std::uint8_t, kMaximumOpusPacket> packet{};
    std::size_t packet_size{};
    std::array<std::int16_t, kMaximumFrameSamples * kMaximumChannels> decoded{};
    std::size_t decoded_samples{}, decoded_offset{};
    std::array<std::uint8_t, kOpusEncoderPcmSamples * sizeof(opus_int16)> pcm_bytes{};
    std::array<opus_int16, kOpusEncoderPcmSamples> pcm{};
    std::size_t pcm_size{};
    std::array<std::uint8_t, kMaximumEncodedPacket> encoded{};
    std::array<std::uint8_t, kWebmHeaderBufferBytes> header{};
};

/** @brief Initializes mono/stereo decoding from a validated OpusHead packet. */
bool init_decoder(OpusStage& s, const std::uint8_t* head, std::size_t size) {
    if (size < kOpusHeadBytes ||
        std::memcmp(head, kOpusHeadSignature.data(), kOpusHeadSignature.size()) != 0 ||
        head[kOpusHeadVersionOffset] > kMaximumCompatibleOpusHeadVersion ||
        (head[kOpusHeadChannelsOffset] != platform::i2s::kMonoChannelCount &&
            head[kOpusHeadChannelsOffset] != kMaximumChannels) ||
        head[kOpusHeadMappingFamilyOffset] != kNoOpusChannelMapping)
        return false;
    int error{};
    s.decoder = opus_decoder_create(kOpusClockRateHz, head[kOpusHeadChannelsOffset], &error);
    if (!s.decoder || error != OPUS_OK)
        return false;
    s.channels = head[kOpusHeadChannelsOffset];
    s.pre_skip_left = static_cast<std::uint32_t>(head[kOpusHeadPreSkipOffset]) |
                      (static_cast<std::uint32_t>(head[kOpusHeadPreSkipOffset + 1U]) << 8U);
    s.format_ready = true;
    return true;
}

/** @brief Resets Ogg page parsing after all segments have been consumed. */
void finish_ogg_page(OpusStage& s) {
    s.ogg_header_size = 0;
    s.ogg_lacing_size = 0;
    s.ogg_lacing_count = 0;
    s.ogg_segment = 0;
    s.ogg_segment_left = 0;
    s.ogg_reading_lacing = false;
    if (s.ogg_page_eos)
        s.eos_seen = true;
}

/** @brief Completes an Ogg packet and retains only Opus headers or audio data. */
bool finish_ogg_packet(OpusStage& s) {
    if (s.ogg_packet_index == kOggOpusHeadPacketIndex) {
        if (!init_decoder(s, s.packet.data(), s.packet_size))
            return false;
        s.packet_size = 0;
        ++s.ogg_packet_index;
        return true;
    }
    if (s.ogg_packet_index == kOggOpusTagsPacketIndex) {
        s.packet_size = 0;
        ++s.ogg_packet_index;
        return true;
    }
    if (!s.packet_size) {
        ++s.ogg_packet_index;
        return true;
    }
    s.packet_ready = true;
    return true;
}

/** @brief Completes empty Ogg segments and advances to the next body segment. */
bool advance_ogg_segments(OpusStage& s) {
    while (s.ogg_segment < s.ogg_lacing_count && s.ogg_segment_left == 0U) {
        const auto lace = s.ogg_lacing[s.ogg_segment++];
        if (lace != kOggContinuedPacketLace && !finish_ogg_packet(s))
            return false;
        if (s.ogg_segment == s.ogg_lacing_count) {
            finish_ogg_page(s);
            return true;
        }
        s.ogg_segment_left = s.ogg_lacing[s.ogg_segment];
    }
    return true;
}

enum class WebmPhase : std::uint8_t { id, size, payload, skip };
enum class WebmStored : std::uint8_t { none, codec_id, codec_private, block };

/** @brief Returns a variable integer's encoded width from its marker bit. */
std::uint8_t vint_width(std::uint8_t first, std::uint8_t maximum) {
    if (!first)
        return 0;
    std::uint8_t mask = 0x80U;
    std::uint8_t width = 1;
    while (!(first & mask) && width <= maximum) {
        mask >>= 1U;
        ++width;
    }
    return width <= maximum ? width : 0;
}

/** @brief Decodes the unmarked value bits from an EBML variable integer. */
std::uint64_t vint_value(const std::uint8_t* bytes, std::uint8_t width) {
    auto value = static_cast<std::uint64_t>(bytes[0] & (0xFFU >> width));
    for (std::uint8_t i = 1; i < width; ++i)
        value = (value << 8U) | bytes[i];
    return value;
}

/** @brief Completes one WebM block and extracts its un-laced Opus packet. */
bool finish_webm_block(OpusStage& s) {
    if (s.webm_store_size < kMinimumWebmBlockHeaderBytes)
        return false;
    const auto track_width = vint_width(s.webm_block[0], kMaximumWebmTrackNumberVintBytes);
    if (!track_width || vint_value(s.webm_block.data(), track_width) != kWebmFirstTrackNumber ||
        s.webm_store_size < static_cast<std::size_t>(track_width) + kWebmBlockTimecodeBytes +
                                kWebmBlockFlagsBytes + kMinimumOpusBlockPayloadBytes ||
        (s.webm_block[static_cast<std::size_t>(track_width) + kWebmBlockTimecodeBytes] &
            kWebmBlockLacingFlags) != 0U)
        return false;
    const auto payload_offset =
        static_cast<std::size_t>(track_width) + kWebmBlockTimecodeBytes + kWebmBlockFlagsBytes;
    const auto payload_size = s.webm_store_size - payload_offset;
    if (!payload_size || payload_size > s.packet.size())
        return false;
    std::copy_n(s.webm_block.data() + payload_offset, payload_size, s.packet.data());
    s.packet_size = payload_size;
    s.packet_ready = true;
    s.webm_store_size = 0;
    return true;
}

/** @brief Pushes a recognized bounded or unknown-size WebM master. */
bool webm_push_master(OpusStage& s, std::uint32_t id, std::uint64_t size, bool unknown) {
    if (s.webm_depth == kMaximumWebmDepth ||
        (!unknown && size > std::numeric_limits<std::uint64_t>::max() - s.webm_position))
        return false;
    auto& master = s.webm_stack[s.webm_depth++];
    master.id = id;
    master.unknown = unknown;
    master.end = unknown ? 0U : s.webm_position + size;
    return true;
}

/** @brief Finalizes an EBML element header and selects a bounded payload action. */
bool webm_begin_element(OpusStage& s) {
    while (s.webm_depth && !s.webm_stack[s.webm_depth - 1U].unknown) {
        const auto end = s.webm_stack[s.webm_depth - 1U].end;
        if (s.webm_element_start > end || (s.webm_element_start < end && s.webm_position > end))
            return false;
        if (s.webm_element_start < end)
            break;
        --s.webm_depth;
    }

    // An unknown-size Cluster ends at the next Cluster. This is the layout
    // emitted by Tasmota and by our bounded writer (one SimpleBlock per cluster).
    if (s.webm_element_id == webm_id(WebmElementId::cluster)) {
        while (s.webm_depth && s.webm_stack[s.webm_depth - 1U].unknown &&
               s.webm_stack[s.webm_depth - 1U].id == webm_id(WebmElementId::cluster))
            --s.webm_depth;
    }

    const auto unknown =
        s.webm_size_length &&
        s.webm_element_size == ((std::uint64_t{1} << (7U * s.webm_size_length)) - 1U);
    const auto id = s.webm_element_id;
    const auto size = s.webm_element_size;
    if (!unknown && size > std::numeric_limits<std::uint64_t>::max() - s.webm_position)
        return false;
    if (s.webm_depth && !s.webm_stack[s.webm_depth - 1U].unknown && !unknown &&
        s.webm_position + size > s.webm_stack[s.webm_depth - 1U].end)
        return false;
    s.webm_store_kind = static_cast<std::uint8_t>(WebmStored::none);
    s.webm_store_size = 0;
    if (id == webm_id(WebmElementId::segment) || id == webm_id(WebmElementId::cluster) ||
        id == webm_id(WebmElementId::tracks) || id == webm_id(WebmElementId::track_entry)) {
        if (unknown && id != webm_id(WebmElementId::segment) &&
            id != webm_id(WebmElementId::cluster))
            return false;
        if (!webm_push_master(s, id, size, unknown))
            return false;
        s.webm_phase = static_cast<std::uint8_t>(WebmPhase::id);
        return true;
    }
    if (unknown)
        return false;
    if (id == webm_id(WebmElementId::codec_id)) {
        if (size > kMaximumWebmCodecIdBytes)
            return false;
        s.webm_store_kind = static_cast<std::uint8_t>(WebmStored::codec_id);
        s.webm_store_size = 0;
    } else if (id == webm_id(WebmElementId::codec_private)) {
        if (size > s.webm_codec_private.size())
            return false;
        s.webm_store_kind = static_cast<std::uint8_t>(WebmStored::codec_private);
        s.webm_store_size = 0;
    } else if (id == webm_id(WebmElementId::simple_block)) {
        if (size > s.webm_block.size() || size < kMinimumWebmBlockHeaderBytes)
            return false;
        s.webm_store_kind = static_cast<std::uint8_t>(WebmStored::block);
        s.webm_store_size = 0;
    }
    s.webm_remaining = size;
    if (!size) {
        if (s.webm_store_kind == static_cast<std::uint8_t>(WebmStored::block) &&
            !finish_webm_block(s))
            return false;
        s.webm_phase = static_cast<std::uint8_t>(WebmPhase::id);
        return true;
    }
    s.webm_phase = s.webm_store_kind == static_cast<std::uint8_t>(WebmStored::none)
                       ? static_cast<std::uint8_t>(WebmPhase::skip)
                       : static_cast<std::uint8_t>(WebmPhase::payload);
    return true;
}

/** @brief Consumes a byte through the minimal streaming WebM/Matroska parser. */
bool feed_webm(OpusStage& s, std::uint8_t byte) {
    const auto phase = static_cast<WebmPhase>(s.webm_phase);
    if (phase == WebmPhase::id) {
        if (!s.webm_id_length) {
            s.webm_id_expected = vint_width(byte, kWebmIdBytes);
            if (!s.webm_id_expected)
                return false;
            s.webm_element_start = s.webm_position;
            s.webm_element_id = byte;
            s.webm_id_length = 1;
        } else {
            if (s.webm_id_length >= s.webm_id_expected)
                return false;
            s.webm_element_id = (s.webm_element_id << 8U) | byte;
            ++s.webm_id_length;
        }
        ++s.webm_position;
        if (s.webm_id_length == s.webm_id_expected) {
            s.webm_phase = static_cast<std::uint8_t>(WebmPhase::size);
            s.webm_id_length = 0;
            s.webm_value = 0;
            s.webm_size_expected = 0;
            s.webm_size_length = 0;
        }
        return true;
    }
    if (phase == WebmPhase::size) {
        if (!s.webm_size_length) {
            s.webm_size_expected = vint_width(byte, kWebmSizeBytes);
            if (!s.webm_size_expected)
                return false;
            s.webm_size_length = 1;
            s.webm_value = byte & (0xFFU >> s.webm_size_expected);
        } else {
            s.webm_value = (s.webm_value << 8U) | byte;
            ++s.webm_size_length;
        }
        ++s.webm_position;
        if (s.webm_size_length == s.webm_size_expected) {
            s.webm_element_size = s.webm_value;
            return webm_begin_element(s);
        }
        return true;
    }

    ++s.webm_position;
    --s.webm_remaining;
    if (phase == WebmPhase::payload) {
        auto* target = s.webm_store_kind == static_cast<std::uint8_t>(WebmStored::block)
                           ? s.webm_block.data()
                       : s.webm_store_kind == static_cast<std::uint8_t>(WebmStored::codec_id)
                           ? s.webm_block.data()
                           : s.webm_codec_private.data();
        const auto capacity = s.webm_store_kind == static_cast<std::uint8_t>(WebmStored::block)
                                  ? s.webm_block.size()
                              : s.webm_store_kind == static_cast<std::uint8_t>(WebmStored::codec_id)
                                  ? s.webm_block.size()
                                  : s.webm_codec_private.size();
        if (s.webm_store_size >= capacity)
            return false;
        target[s.webm_store_size++] = byte;
    }
    if (!s.webm_remaining) {
        if (s.webm_store_kind == static_cast<std::uint8_t>(WebmStored::codec_id)) {
            if (s.webm_store_size != kWebmOpusCodecId.size() ||
                std::memcmp(
                    s.webm_block.data(), kWebmOpusCodecId.data(), kWebmOpusCodecId.size()) != 0)
                return false;
        } else if (s.webm_store_kind == static_cast<std::uint8_t>(WebmStored::codec_private)) {
            if (!init_decoder(s, s.webm_codec_private.data(), s.webm_store_size))
                return false;
        } else if (s.webm_store_kind == static_cast<std::uint8_t>(WebmStored::block) &&
                   !finish_webm_block(s))
            return false;
        s.webm_phase = static_cast<std::uint8_t>(WebmPhase::id);
        s.webm_store_kind = static_cast<std::uint8_t>(WebmStored::none);
        s.webm_store_size = 0;
    }
    return true;
}

/** @brief Consumes one byte of a bounded Ogg page and assembles complete packets. */
bool feed_ogg(OpusStage& s, std::uint8_t value) {
    if (s.ogg_header_size < s.ogg_header.size()) {
        s.ogg_header[s.ogg_header_size++] = value;
        if (s.ogg_header_size == s.ogg_header.size()) {
            const auto* h = s.ogg_header.data();
            if (std::memcmp(h, kOggCapturePattern.data(), kOggCapturePattern.size()) != 0 ||
                h[kOggPageVersionOffset] != 0U)
                return false;
            s.ogg_page_eos = (h[kOggPageHeaderTypeOffset] & kOggPageEndOfStreamFlag) != 0U;
            s.ogg_lacing_count = h[kOggPageSegmentCountOffset];
            s.ogg_reading_lacing = true;
            if (!s.ogg_lacing_count)
                finish_ogg_page(s);
        }
        return true;
    }
    if (s.ogg_reading_lacing) {
        s.ogg_lacing[s.ogg_lacing_size++] = value;
        if (s.ogg_lacing_size == s.ogg_lacing_count) {
            s.ogg_reading_lacing = false;
            s.ogg_segment = 0;
            s.ogg_segment_left = s.ogg_lacing[0];
            if (!advance_ogg_segments(s))
                return false;
        }
        return true;
    }

    const auto packet_number = s.ogg_packet_index;
    if (packet_number != kOggOpusTagsPacketIndex) {
        if (s.packet_size == s.packet.size())
            return false;
        s.packet[s.packet_size++] = value;
    }
    if (s.ogg_segment_left)
        --s.ogg_segment_left;
    if (s.ogg_segment_left)
        return true;

    return advance_ogg_segments(s);
}

std::size_t drain_decoded(OpusStage& s, std::uint8_t* out, std::size_t capacity) {
    const auto total = s.decoded_samples * s.channels * sizeof(std::int16_t);
    const auto remaining = total - s.decoded_offset;
    const auto frame_bytes = static_cast<std::size_t>(s.channels) * sizeof(std::int16_t);
    const auto count = std::min(remaining, capacity - capacity % frame_bytes);
    if (count) {
        auto source_offset = s.decoded_offset / sizeof(std::int16_t);
        for (std::size_t n = 0; n < count; n += sizeof(std::int16_t)) {
            const auto sample = static_cast<std::uint16_t>(s.decoded[source_offset++]);
            out[n] = static_cast<std::uint8_t>(sample);
            out[n + 1U] = static_cast<std::uint8_t>(sample >> 8U);
        }
        s.decoded_offset += count;
    }
    if (s.decoded_offset == total)
        s.decoded_offset = s.decoded_samples = 0;
    return count;
}

/** @brief Decodes one complete Ogg or WebM packet into a retained PCM window. */
Result decode_packet(OpusStage& s) {
    const auto samples = opus_packet_get_nb_samples(
        s.packet.data(), static_cast<opus_int32>(s.packet_size), kOpusClockRateHz);
    if (samples <= 0 || static_cast<std::size_t>(samples) > kMaximumFrameSamples)
        return Result::codec_error;
    const auto decoded = opus_decode(s.decoder, s.packet.data(),
        static_cast<opus_int32>(s.packet_size), s.decoded.data(), samples, 0);
    if (decoded < 0)
        return Result::codec_error;
    s.decoded_samples = static_cast<std::size_t>(decoded);
    const auto skip =
        std::min<std::uint32_t>(s.pre_skip_left, static_cast<std::uint32_t>(s.decoded_samples));
    s.pre_skip_left -= skip;
    s.decoded_offset = static_cast<std::size_t>(skip) * s.channels * sizeof(std::int16_t);
    s.packet_ready = false;
    s.packet_size = 0;
    ++s.ogg_packet_index;
    return Result::success;
}

/** @brief Incrementally sniffs and decodes Ogg Opus or un-laced WebM Opus. */
Step decode_process(OpusStage& s, const std::uint8_t* input, std::size_t size, bool eof,
    std::uint8_t* output, std::size_t capacity) {
    Step step{};
    if (s.decoded_samples) {
        step.produced = drain_decoded(s, output, capacity);
        step.sample_rate = kOpusClockRateHz;
        step.channels = s.channels;
        return step;
    }
    if (s.packet_ready) {
        step.result = decode_packet(s);
        if (step.result != Result::success)
            return step;
        step.produced = drain_decoded(s, output, capacity);
        step.sample_rate = kOpusClockRateHz;
        step.channels = s.channels;
        return step;
    }

    while (step.consumed < size) {
        const auto byte = input[step.consumed++];
        if (s.format == Format::unknown) {
            s.sniff[s.sniff_size++] = byte;
            if (s.sniff_size == s.sniff.size()) {
                bool accepted{};
                if (std::memcmp(s.sniff.data(), kOggCapturePattern.data(),
                        kOggCapturePattern.size()) == 0) {
                    s.format = Format::ogg;
                    accepted = true;
                    for (const auto initial : s.sniff)
                        if (!feed_ogg(s, initial)) {
                            step.result = Result::codec_error;
                            return step;
                        }
                } else if (std::equal(kWebmEbmlSignature.begin(), kWebmEbmlSignature.end(),
                               s.sniff.begin())) {
                    s.format = Format::webm;
                    accepted = true;
                    for (const auto initial : s.sniff)
                        if (!feed_webm(s, initial)) {
                            step.result = Result::codec_error;
                            return step;
                        }
                }
                if (!accepted) {
                    step.result = Result::codec_error;
                    return step;
                }
            }
        } else if ((s.format == Format::ogg && !feed_ogg(s, byte)) ||
                   (s.format == Format::webm && !feed_webm(s, byte))) {
            step.result = Result::codec_error;
            return step;
        }
        if (s.packet_ready) {
            step.result = decode_packet(s);
            if (step.result != Result::success)
                return step;
            step.produced = drain_decoded(s, output, capacity);
            step.sample_rate = kOpusClockRateHz;
            step.channels = s.channels;
            return step;
        }
    }

    if (eof) {
        if ((s.format == Format::ogg && (s.ogg_header_size || s.ogg_reading_lacing ||
                                            s.ogg_segment_left || s.packet_size)) ||
            (s.format == Format::webm &&
                (s.webm_phase != static_cast<std::uint8_t>(WebmPhase::id) || s.webm_id_length ||
                    !s.format_ready)) ||
            (s.format == Format::unknown && s.sniff_size)) {
            step.result = Result::codec_error;
            return step;
        }
        step.finished = true;
    }
    return step;
}

/** @brief Emits Tasmota's 20 ms Opus frames into bounded WebM clusters. */
Step encode_process(OpusStage& s, const std::uint8_t* input, std::size_t size, bool eof,
    std::uint8_t* output, std::size_t capacity) {
    Step step{};
    if (!s.header_written) {
        opus_int32 lookahead{};
        if (opus_encoder_ctl(s.encoder, OPUS_GET_LOOKAHEAD(&lookahead)) != OPUS_OK || lookahead < 0)
            step.result = Result::codec_error;
        else {
            const auto pre_skip = static_cast<std::uint16_t>(
                (static_cast<std::uint64_t>(lookahead) * kOpusClockRateHz + s.sample_rate / 2U) /
                s.sample_rate);
            const auto delay =
                static_cast<std::uint64_t>(lookahead) * kNanosecondsPerSecond / s.sample_rate;
            std::size_t header_size{};
            if (!make_webm_header(s.header.data(), s.header.size(), header_size, s.sample_rate,
                    s.channels, pre_skip, delay) ||
                header_size > capacity)
                step.result = Result::codec_error;
            else {
                std::memcpy(output, s.header.data(), header_size);
                step.produced = header_size;
                s.header_written = true;
            }
        }
        return step;
    }

    const auto frame_bytes = static_cast<std::size_t>(s.frame_samples) * s.channels * 2U;
    if (s.pcm_size < frame_bytes) {
        const auto count = std::min(size, frame_bytes - s.pcm_size);
        if (count) {
            std::memcpy(s.pcm_bytes.data() + s.pcm_size, input, count);
            s.pcm_size += count;
            step.consumed = count;
        }
    }
    if (s.pcm_size < frame_bytes) {
        if (eof) {
            // Tasmota finalizes on complete 20 ms packets. A short tail is dropped;
            // encoding a padded packet would require Matroska DiscardPadding.
            s.pcm_size = 0;
            step.finished = true;
        }
        return step;
    }

    const auto sample_count = static_cast<std::size_t>(s.frame_samples) * s.channels;
    for (std::size_t n = 0; n < sample_count; ++n) {
        const auto value = static_cast<std::uint16_t>(s.pcm_bytes[n * 2U]) |
                           static_cast<std::uint16_t>(s.pcm_bytes[n * 2U + 1U] << 8U);
        s.pcm[n] = static_cast<opus_int16>(value);
    }
    const auto encoded = opus_encode(s.encoder, s.pcm.data(), static_cast<int>(s.frame_samples),
        s.encoded.data(), static_cast<opus_int32>(s.encoded.size()));
    if (encoded < 0) {
        step.result = Result::codec_error;
        return step;
    }
    s.pcm_size = 0;
    if (encoded >= kMinimumMuxedOpusPacketBytes) {
        const auto cluster_ms = (s.time_ms / kWebmClusterMilliseconds) * kWebmClusterMilliseconds;
        std::size_t block_size{};
        if (!make_cluster(output, capacity, block_size, cluster_ms,
                static_cast<std::uint16_t>(s.time_ms - cluster_ms), s.encoded.data(),
                static_cast<std::size_t>(encoded))) {
            step.result = Result::codec_error;
            return step;
        }
        step.produced = block_size;
    }
    s.time_ms += kOpusPacketDurationMilliseconds;
    if (eof && step.consumed == size)
        step.finished = true;
    return step;
}

/** @brief Dispatches one bounded codec step for the active direction. */
Step process(void* context, const std::uint8_t* input, std::size_t size, bool eof,
    std::uint8_t* output, std::size_t capacity) {
    auto& s = *static_cast<OpusStage*>(context);
    return s.encode ? encode_process(s, input, size, eof, output, capacity)
                    : decode_process(s, input, size, eof, output, capacity);
}

/** @brief Resets an Opus decoder before replaying a file loop. */
Result reset(void* context) {
    auto& s = *static_cast<OpusStage*>(context);
    if (s.encode)
        return Result::unsupported;
    if (s.decoder) {
        opus_decoder_destroy(s.decoder);
        s.decoder = nullptr;
    }
    const auto config = s.config;
    s.~OpusStage();
    new (&s) OpusStage{};
    s.config = config;
    return Result::success;
}

/** @brief Releases Opus state and its bounded packet/PCM storage. */
void close(void* context) {
    auto* s = static_cast<OpusStage*>(context);
    if (s->encoder)
        opus_encoder_destroy(s->encoder);
    if (s->decoder)
        opus_decoder_destroy(s->decoder);
    delete s;
}
} // namespace

/** @brief Opens carried fixed-point Opus with Tasmota's complexity/frame tuning. */
Result open_opus(const Config& config, bool encode, Stage& stage) {
    stage = {};
    auto* s = new (std::nothrow) OpusStage{};
    if (!s)
        return Result::no_memory;
    s->config = config;
    s->encode = encode;
    if (encode) {
        const auto channels = platform::i2s::channel_count(config.input);
        const auto rate = config.input.sample_rate;
        if (!channels || channels > kMaximumChannels ||
            config.input.sample_bits != platform::i2s::SampleWidth::bits16 ||
            std::find(kSupportedEncoderRatesHz.begin(), kSupportedEncoderRatesHz.end(), rate) ==
                kSupportedEncoderRatesHz.end()) {
            delete s;
            return Result::unsupported;
        }
        int error{};
        const auto application =
            rate > kOpusVoiceRateLimitHz ? OPUS_APPLICATION_AUDIO : OPUS_APPLICATION_VOIP;
        s->encoder = opus_encoder_create(
            static_cast<opus_int32>(rate), static_cast<int>(channels), application, &error);
        if (!s->encoder || error != OPUS_OK) {
            if (s->encoder)
                opus_encoder_destroy(s->encoder);
            delete s;
            return error == OPUS_ALLOC_FAIL ? Result::no_memory : Result::codec_error;
        }
        // Preserve the proven Tasmota MCU tuning for the carried Opus 1.3.1 core.
        if (opus_encoder_ctl(s->encoder, OPUS_SET_COMPLEXITY(kTasmotaEncoderComplexity)) !=
            OPUS_OK) {
            opus_encoder_destroy(s->encoder);
            delete s;
            return Result::codec_error;
        }
        s->sample_rate = rate;
        s->channels = static_cast<std::uint8_t>(channels);
        s->frame_samples = rate * kOpusPacketDurationMilliseconds / kMillisecondsPerSecond;
    }
    stage = {s, process, reset, close, encode ? s->frame_samples : 0U};
    return Result::success;
}
} // namespace newmota::audio::detail

#else
namespace newmota::audio::detail {
Result open_opus(const Config&, bool, Stage&) {
    return Result::unsupported;
}
} // namespace newmota::audio::detail
#endif
