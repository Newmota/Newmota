/*
  audio_settings.h - Persistent I2S command profiles

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_settings.h
 * @brief Pin-free I2S profile values kept by the settings file.
 *
 * Pins always come from the hardware template, so a profile never stores them: a template
 * change cannot leave a stale pad behind. A profile that was never set stays absent and the
 * compiled defaults apply. Values are plain integers so the record survives enum reordering.
 */
#pragma once
#include "newmota/platform/i2s.h"
#include <array>
#include <cstdint>

namespace newmota {
/** @brief Output volume in percent and microphone gain in Q12.4 (16 is unity). */
inline constexpr std::uint16_t kDefaultOutputGainPercent = 100;
inline constexpr std::uint16_t kMaximumOutputGainPercent = 100;
inline constexpr std::uint16_t kUnityInputGain = 16;
inline constexpr std::uint16_t kMaximumInputGain = 256 * kUnityInputGain;

/** @brief Tuning flags, one bit each, in this order from bit 0. */
inline constexpr std::uint8_t kTuningWsPol = 1U << 0U;
inline constexpr std::uint8_t kTuningLeftAlign = 1U << 1U;
inline constexpr std::uint8_t kTuningBigEndian = 1U << 2U;
inline constexpr std::uint8_t kTuningLsbFirst = 1U << 3U;
inline constexpr std::uint8_t kTuningMclkInv = 1U << 4U;
inline constexpr std::uint8_t kTuningBclkInv = 1U << 5U;
inline constexpr std::uint8_t kTuningWsInv = 1U << 6U;
inline constexpr std::uint8_t kTuningApll = 1U << 7U;

/** @brief One direction's persisted wire profile and gain. */
struct I2sProfileSettings final {
    /** @brief False selects the compiled defaults; the other fields are then ignored. */
    bool present{};
    std::uint8_t mode{}, role{}, msb_right{};
    std::uint8_t sample_bits{16}, slot_bits{16};
    bool pdm_pcm{true};
    std::uint32_t sample_rate{platform::i2s::kDefaultSampleRateHz};
    std::uint16_t slot_mask{static_cast<std::uint16_t>(platform::i2s::kStereoSlotMask)};
    std::uint16_t dma_frames{platform::i2s::kDefaultDmaFrames};
    /** @brief PDM clock pad, or `PinId::kInvalid` to route the clock through WS. */
    std::uint16_t pdm_clock{0xffffU};
    std::uint8_t framing{};
    /** @brief Transmit: volume percent. Receive: microphone gain in Q12.4. */
    std::uint16_t gain{kDefaultOutputGainPercent};
    std::uint8_t ws_width{};
    std::uint8_t tuning_flags{kTuningLeftAlign};
    std::uint8_t dma_descriptors{platform::i2s::kDefaultDmaDescriptors};

    /** @brief Rejects values the portable configuration boundary would refuse. */
    [[nodiscard]] constexpr bool valid() const {
        const auto width = [](std::uint8_t bits) { return bits == 16 || bits == 24 || bits == 32; };
        return mode <= 2 && role <= 1 && msb_right <= 2 && width(sample_bits) && width(slot_bits) &&
               sample_rate != 0 && sample_rate <= platform::i2s::kMaximumRawPdmRateHz &&
               slot_mask != 0 && dma_frames != 0 &&
               dma_frames <= platform::i2s::kMaximumDmaFrames && framing <= 3 &&
               gain <= kMaximumInputGain &&
               dma_descriptors >= platform::i2s::kMinimumDmaDescriptors &&
               dma_descriptors <= platform::i2s::kMaximumDmaDescriptors;
    }

    [[nodiscard]] constexpr bool operator==(const I2sProfileSettings& other) const {
        return present == other.present && mode == other.mode && role == other.role &&
               msb_right == other.msb_right && sample_bits == other.sample_bits &&
               slot_bits == other.slot_bits && pdm_pcm == other.pdm_pcm &&
               sample_rate == other.sample_rate && slot_mask == other.slot_mask &&
               dma_frames == other.dma_frames && pdm_clock == other.pdm_clock &&
               framing == other.framing && gain == other.gain && ws_width == other.ws_width &&
               tuning_flags == other.tuning_flags && dma_descriptors == other.dma_descriptors;
    }
};

/** @brief Persisted `I2SConfig` defaults for both directions. */
struct AudioSettings final {
    static constexpr std::size_t kProfileEncodedSize = 23;
    static constexpr std::size_t kRecordSize = 2 * kProfileEncodedSize;

    I2sProfileSettings transmit{};
    I2sProfileSettings receive{receive_defaults()};

    static constexpr I2sProfileSettings receive_defaults() {
        I2sProfileSettings p{};
        p.gain = kUnityInputGain;
        return p;
    }

    [[nodiscard]] constexpr bool valid() const {
        return (!transmit.present ||
                   (transmit.valid() && transmit.gain <= kMaximumOutputGainPercent)) &&
               (!receive.present || receive.valid());
    }

    [[nodiscard]] constexpr bool operator==(const AudioSettings& other) const {
        return transmit == other.transmit && receive == other.receive;
    }
};
} // namespace newmota
