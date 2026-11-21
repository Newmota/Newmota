/*
  i2s.h - Native I2S block transport contract

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file i2s.h
 * @brief Native audio transport; no codec, container, or filesystem ownership.
 */
#pragma once
#include "newmota/core/hardware/pin.h"
#include <cstddef>
#include <cstdint>

namespace newmota::platform::i2s {
enum class Result : std::uint8_t {
    success,
    invalid_argument,
    unsupported,
    busy,
    no_memory,
    timeout,
    failed
};
enum class Mode : std::uint8_t { standard, tdm, pdm };
/** @brief Data alignment to WS. `pcm_short` is a one-BCLK WS pulse with Philips data delay. */
enum class Framing : std::uint8_t { philips, left_justified, right_justified, pcm_short };
enum class Role : std::uint8_t { controller, target };
enum class Direction : std::uint8_t { receive, transmit };
/** @brief Explicit HW-v1 FIFO placement override; automatic preserves SDK SoC defaults. */
enum class MsbRight : std::uint8_t { automatic, disabled, enabled };
/** @brief PCM sample and wire slot widths supported by the portable contract. */
enum class SampleWidth : std::uint8_t { bits16 = 16, bits24 = 24, bits32 = 32 };

/** @brief Masks and defaults for the common two-slot I2S frame. */
inline constexpr std::uint32_t kLeftSlotMask = 1U;
inline constexpr std::uint32_t kRightSlotMask = 2U;
inline constexpr std::uint32_t kStereoSlotMask = kLeftSlotMask | kRightSlotMask;
inline constexpr std::uint8_t kMonoChannelCount = 1;
inline constexpr std::uint8_t kStereoChannelCount = 2;
inline constexpr std::uint8_t kStandardSlotCount = 2;
/** @brief Default stream rate and native DMA frame count for a new lease. */
inline constexpr std::uint32_t kDefaultSampleRateHz = 16000;
inline constexpr std::uint16_t kDefaultDmaFrames = 128;
inline constexpr std::uint8_t kDefaultDmaDescriptors = 4;
inline constexpr std::uint8_t kMinimumDmaDescriptors = 2;
inline constexpr std::uint8_t kMaximumDmaDescriptors = 16;
/** @brief Bounds accepted by the portable configuration boundary; SoCs may be narrower. */
inline constexpr std::uint16_t kMaximumDmaFrames = 512;
inline constexpr std::uint32_t kMaximumPcmRateHz = 192000;
inline constexpr std::uint32_t kMaximumRawPdmRateHz = 6144000;

/** @brief Returns the numeric width needed by native SDK configuration fields. */
constexpr std::uint8_t sample_width_bits(SampleWidth width) {
    return static_cast<std::uint8_t>(width);
}

/** @brief Returns the portable little-endian storage bytes for one sample. */
constexpr std::size_t sample_storage_bytes(SampleWidth width) {
    return width == SampleWidth::bits16 ? sizeof(std::uint16_t) : sizeof(std::uint32_t);
}

/** @brief Rejects invalid enum values supplied through an external configuration boundary. */
constexpr bool valid_sample_width(SampleWidth width) {
    return width == SampleWidth::bits16 || width == SampleWidth::bits24 ||
           width == SampleWidth::bits32;
}

/** @brief ESP-IDF slot, pin and clock options, the register-level part of Tasmota's I2SConfig.
 * Backends whose capabilities() report no `tuning` reject anything but these defaults; the
 * serial-order fields also need the ESP32-C3/S3 generation (not ESP32/S2). PDM uses `ws_inv`
 * for its clock, as Tasmota does.
 */
struct Tuning {
    /** @brief WS pulse width in BCLK cycles; 0 follows the framing. */
    std::uint8_t ws_width{};
    bool ws_pol{}, left_align{true}, big_endian{}, lsb_first{};
    bool mclk_inv{}, bclk_inv{}, ws_inv{};
    /** @brief Clocks from the audio PLL where the SoC has one. */
    bool apll{};
    std::uint8_t dma_descriptors{kDefaultDmaDescriptors};
};

constexpr bool operator==(const Tuning& a, const Tuning& b) {
    return a.ws_width == b.ws_width && a.ws_pol == b.ws_pol && a.left_align == b.left_align &&
           a.big_endian == b.big_endian && a.lsb_first == b.lsb_first && a.mclk_inv == b.mclk_inv &&
           a.bclk_inv == b.bclk_inv && a.ws_inv == b.ws_inv && a.apll == b.apll &&
           a.dma_descriptors == b.dma_descriptors;
}

/** @brief Wire configuration. Directions are selected by valid DIN/DOUT pads.
 * PCM samples use native little endian words: 16 bits occupy 2 bytes, 24/32 bits
 * occupy 4 bytes (24-bit samples are left aligned). Frames contain selected
 * slots in ascending order. Raw PDM is a packed bitstream, explicitly selected with pdm_pcm=false;
 * it is not signed PCM.
 * A paired open owns both directions and their common clocks as one lease.
 */
struct Config {
    std::int8_t controller{-1};
    Mode mode{Mode::standard};
    Framing framing{Framing::philips};
    Role role{Role::controller};
    std::uint32_t sample_rate{kDefaultSampleRateHz};
    SampleWidth sample_bits{SampleWidth::bits16}, slot_bits{SampleWidth::bits16};
    std::uint8_t slots{kStandardSlotCount};
    std::uint32_t slot_mask{kStereoSlotMask};
    PinId bclk{}, ws{}, dout{}, din{}, mclk{};
    /** @brief PDM clock override. When absent, WS is the explicitly selected clock route. */
    PinId pdm_clock{};
    MsbRight msb_right{MsbRight::automatic};
    std::uint16_t dma_frames{kDefaultDmaFrames};
    bool pdm_pcm{true};
    Tuning tuning{};
};

/** @brief Coarse SoC capabilities; open validates controller and route specifics. */
/** @brief PDM has one clock, independent of the standard BCLK signal. */
constexpr PinId pdm_clock_pin(const Config& c) {
    return c.pdm_clock.valid() ? c.pdm_clock : c.ws;
}

struct Capabilities {
    std::uint8_t controllers{};
    std::uint32_t modes{};
    bool tx{}, rx{}, duplex{}, pdm_tx_pcm{}, pdm_rx_pcm{}, pdm_tx{}, pdm_rx{};
    /** @brief Accepts non-default Tuning; `apll` additionally needs `apll`, and left_align,
     * big_endian and lsb_first need `serial_order`. */
    bool tuning{}, apll{}, serial_order{};
};

struct Transfer {
    Result result{Result::success};
    std::size_t count{};
};

struct Status {
    std::uint32_t received{}, transmitted{}, overruns{}, underruns{};
    std::uint32_t actual_rate{};
};

using Port = std::uint8_t;

/** @brief Checks that a value crossing an enum boundary names a supported option. */
constexpr bool valid_mode(Mode mode) {
    return mode == Mode::standard || mode == Mode::tdm || mode == Mode::pdm;
}

/** @brief Rejects invalid framing values at configuration boundaries. */
constexpr bool valid_framing(Framing framing) {
    return framing == Framing::philips || framing == Framing::left_justified ||
           framing == Framing::right_justified || framing == Framing::pcm_short;
}

/** @brief Rejects invalid controller/target role values at configuration boundaries. */
constexpr bool valid_role(Role role) {
    return role == Role::controller || role == Role::target;
}

/** @brief Returns a bitmask value for a wire mode. */
constexpr std::uint32_t mode_mask(Mode m) {
    return valid_mode(m) ? 1U << unsigned(m) : 0U;
}

/** @brief Number of active memory slots in a frame. */
constexpr unsigned channel_count(const Config& c) {
    unsigned count{};
    for (auto mask = c.slot_mask; mask; mask >>= 1U)
        count += mask & 1U;
    return count;
}

/** @brief Memory bytes per whole frame, distinct from the wire slot width. */
constexpr std::size_t frame_bytes(const Config& c) {
    return channel_count(c) * sample_storage_bytes(c.sample_bits);
}

/** @brief Returns capabilities for this compiled SoC. */
Capabilities capabilities();
/** @brief Acquires DMA, clocks and directions atomically, or rolls back.
 * Calls belong to the application task. `failed` means cleanup is uncertain;
 * the application must retain pad claims. Other failures leave no resources.
 */
Result open(const Config&, Port&);
/** @brief Aborts queued output, stops capture and detaches pins before success. */
Result close(Port);
/** @brief Reports when every accepted TX frame has left the native DMA pipeline. */
bool tx_idle(Port);
/** @brief Copies currently captured whole frames without waiting. */
Transfer read(Port, std::uint8_t*, std::size_t);
/** @brief Accepts complete frames without waiting; caller retries short writes. */
Transfer write(Port, const std::uint8_t*, std::size_t);
/** @brief Returns cumulative byte and loss counters plus negotiated sample rate. */
Status status(Port);
/** @brief Performs optional cooperative native maintenance without blocking. */
void poll();
} // namespace newmota::platform::i2s
