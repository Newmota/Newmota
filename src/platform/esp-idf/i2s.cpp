/*
  i2s.cpp - ESP-IDF channel based audio transport

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file i2s.cpp
 * @brief Independent simplex leases or paired standard/TDM full duplex leases.
 */
#include "newmota/platform/i2s.h"
#include "newmota/platform/gpio.h"
#include "soc/soc_caps.h"
#include <array>
#include <atomic>
#include <algorithm>
#include <cstring>
#if SOC_I2S_SUPPORTED
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "driver/i2s_pdm.h"
#include "esp_attr.h"
#if !defined(SOC_I2S_NUM)
#include "hal/i2s_ll.h"
#define SOC_I2S_NUM I2S_LL_GET(INST_NUM)
#endif
#endif

namespace newmota::platform::i2s {
#if SOC_I2S_SUPPORTED
namespace {
constexpr std::size_t kChannelsPerController = 2;
constexpr std::size_t kSampleCacheBytes = 256;
constexpr std::size_t kMono16SampleBytes = sizeof(std::uint16_t);
constexpr std::size_t kMono16DmaGroupBytes = 2 * kMono16SampleBytes;
constexpr std::size_t kPcm24PackedBytesPerSample = 3;
constexpr std::size_t kPcm24StorageBytesPerSample = 4;
constexpr std::size_t kPcm24LeadingPaddingBytes = 1;
constexpr std::size_t kPcm24SamplesPerBatch = 64;
constexpr std::uint32_t kPcm16DmaFrameAlignment = 2;
constexpr std::uint32_t kPcm24DmaFrameAlignment = 3;
constexpr std::uint32_t kMaximumDmaPayloadBytes = 4092;
constexpr std::uint32_t kNonblockingTimeoutTicks = 0;
constexpr std::int32_t kFirstController = 0;
constexpr std::int32_t kNoController = -1;

struct State {
    i2s_chan_handle_t tx{}, rx{};
    int controller{kNoController};
    Config config{};
    std::atomic<std::uint32_t> overruns{}, underruns{};
    std::atomic<std::uint32_t> sent_pages{};
    std::uint32_t written_epoch{};
    bool written{};
    // Original ESP32 standard mono16 DMA exchanges adjacent sample words.
    // The portable boundary always preserves chronological sample order.
    bool swap_mono{}, packed24{}, tx_odd{};
    std::uint8_t tx_one[sizeof(std::uint16_t)]{}, tx_cache[kSampleCacheBytes]{},
        rx_cache[kSampleCacheBytes]{};
    std::size_t tx_size{}, tx_offset{}, rx_size{}, rx_offset{};
    std::atomic<std::uint32_t> received{}, transmitted{};
    bool occupied{}, tx_enabled{}, rx_enabled{};
};

std::array<State, SOC_I2S_NUM * kChannelsPerController> states{};

/** @brief Maps SDK validation/allocation failures without discarding ownership. */
Result translate(esp_err_t err) {
    switch (err) {
    case ESP_OK:
        return Result::success;
    case ESP_ERR_INVALID_ARG:
        return Result::invalid_argument;
    case ESP_ERR_NOT_SUPPORTED:
        return Result::unsupported;
    case ESP_ERR_NOT_FOUND:
    case ESP_ERR_INVALID_STATE:
        return Result::busy;
    case ESP_ERR_NO_MEM:
        return Result::no_memory;
    case ESP_ERR_TIMEOUT:
        return Result::timeout;
    default:
        return Result::failed;
    }
}

gpio_num_t pad(PinId p) {
    return p.valid() ? static_cast<gpio_num_t>(p.value()) : I2S_GPIO_UNUSED;
}

bool IRAM_ATTR overflow(i2s_chan_handle_t, i2s_event_data_t*, void* context) {
    static_cast<State*>(context)->overruns.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool IRAM_ATTR starvation(i2s_chan_handle_t, i2s_event_data_t*, void* context) {
    static_cast<State*>(context)->underruns.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool IRAM_ATTR sent(i2s_chan_handle_t, i2s_event_data_t*, void* context) {
    static_cast<State*>(context)->sent_pages.fetch_add(1, std::memory_order_relaxed);
    return false;
}

/** @brief Normalizes the original ESP32 mono16 DMA word order in bounded blocks. */
void exchange_samples(std::uint8_t* data, std::size_t size) {
    for (std::size_t i = 0; i < size; i += kMono16DmaGroupBytes) {
        std::swap(data[i], data[i + kMono16SampleBytes]);
        std::swap(data[i + 1], data[i + kMono16SampleBytes + 1]);
    }
}

/** @brief Publishes a native write and records a conservative DMA drain fence. */
esp_err_t send(State& s, const std::uint8_t* data, std::size_t size, std::size_t& count) {
    const auto err = i2s_channel_write(s.tx, data, size, &count, kNonblockingTimeoutTicks);
    if (count) {
        s.written_epoch = s.sent_pages.load(std::memory_order_relaxed);
        s.written = true;
    }
    return err;
}

/** @brief Flushes a previously normalized short TX block without waiting. */
esp_err_t pump(State& s) {
    if (s.tx_offset == s.tx_size)
        return ESP_OK;
    std::size_t count{};
    const auto err = send(s, s.tx_cache + s.tx_offset, s.tx_size - s.tx_offset, count);
    s.tx_offset += count;
    return err == ESP_ERR_TIMEOUT ? ESP_OK : err;
}

/** @brief Deletes every successfully acquired channel, including partially initialized ones. */
bool release(State& s) {
    bool success = true;
    if (s.tx_enabled) {
        if (i2s_channel_disable(s.tx) == ESP_OK)
            s.tx_enabled = false;
        else
            success = false;
    }
    if (s.rx_enabled) {
        if (i2s_channel_disable(s.rx) == ESP_OK)
            s.rx_enabled = false;
        else
            success = false;
    }
    if (s.tx && !s.tx_enabled) {
        if (i2s_del_channel(s.tx) == ESP_OK)
            s.tx = nullptr;
        else
            success = false;
    }
    if (s.rx && !s.rx_enabled) {
        if (i2s_del_channel(s.rx) == ESP_OK)
            s.rx = nullptr;
        else
            success = false;
    }
    return success;
}

/** @brief Applies the Tuning slot options on top of the framing's defaults.
 * Only changes from the defaults are applied, so TDM keeps its SDK serial order.
 */
template <typename Slot> void tune_slot(Slot& slot, const Tuning& t) {
    if (t.ws_width)
        slot.ws_width = t.ws_width;
    slot.ws_pol = slot.ws_pol != t.ws_pol; // relative to the framing, as PCM starts high
#if SOC_I2S_HW_VERSION_2
    if (!t.left_align)
        slot.left_align = false;
    if (t.big_endian)
        slot.big_endian = true;
    if (t.lsb_first)
        slot.bit_order_lsb = true;
#endif
}

template <typename Clock> void tune_clock(Clock& clock, const Tuning& t) {
#if SOC_I2S_SUPPORTS_APLL
    if (t.apll)
        clock.clk_src = I2S_CLK_SRC_APLL;
#else
    (void)clock;
    (void)t;
#endif
}

template <typename Gpio> void invert_pins(Gpio& gpio, const Tuning& t) {
    gpio.invert_flags.mclk_inv = t.mclk_inv;
    gpio.invert_flags.bclk_inv = t.bclk_inv;
    gpio.invert_flags.ws_inv = t.ws_inv;
}

/** @brief Configures exactly the selected mode without falling back to standard I2S. */
esp_err_t initialize(i2s_chan_handle_t channel, const Config& c, bool tx) {
    const auto width = static_cast<i2s_data_bit_width_t>(sample_width_bits(c.sample_bits));
    const auto slots =
        channel_count(c) == kMonoChannelCount ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;
    if (c.mode == Mode::standard) {
        i2s_std_config_t cfg{};
        cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(c.sample_rate);
        // 24-bit slots require an MCLK multiple divisible by three.
        if (c.slot_bits == SampleWidth::bits24)
            cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
        if (c.framing == Framing::pcm_short)
            cfg.slot_cfg = I2S_STD_PCM_SLOT_DEFAULT_CONFIG(width, slots);
        else {
            cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(width, slots);
            cfg.slot_cfg.ws_width = sample_width_bits(c.slot_bits);
        }
        cfg.slot_cfg.slot_bit_width =
            static_cast<i2s_slot_bit_width_t>(sample_width_bits(c.slot_bits));
        cfg.slot_cfg.slot_mask = static_cast<i2s_std_slot_mask_t>(c.slot_mask);
#if SOC_I2S_HW_VERSION_1
        // The field controls FIFO placement on ESP32/S2, not serial LSB order.
        if (c.msb_right != MsbRight::automatic)
            cfg.slot_cfg.msb_right = c.msb_right == MsbRight::enabled;
#endif
        if (c.framing == Framing::left_justified)
            cfg.slot_cfg.bit_shift = false;
        tune_slot(cfg.slot_cfg, c.tuning);
        tune_clock(cfg.clk_cfg, c.tuning);
        cfg.gpio_cfg.mclk = pad(c.mclk);
        cfg.gpio_cfg.bclk = pad(c.bclk);
        cfg.gpio_cfg.ws = pad(c.ws);
        cfg.gpio_cfg.dout = pad(c.dout);
        cfg.gpio_cfg.din = pad(c.din);
        invert_pins(cfg.gpio_cfg, c.tuning);
        return i2s_channel_init_std_mode(channel, &cfg);
    }
#if SOC_I2S_SUPPORTS_TDM
    if (c.mode == Mode::tdm) {
        i2s_tdm_config_t cfg{};
        cfg.clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(c.sample_rate);
        if (c.framing == Framing::pcm_short)
            cfg.slot_cfg = I2S_TDM_PCM_SHORT_SLOT_DEFAULT_CONFIG(
                width, slots, static_cast<i2s_tdm_slot_mask_t>(c.slot_mask));
        else
            cfg.slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
                width, slots, static_cast<i2s_tdm_slot_mask_t>(c.slot_mask));
        cfg.slot_cfg.slot_bit_width =
            static_cast<i2s_slot_bit_width_t>(sample_width_bits(c.slot_bits));
        cfg.slot_cfg.total_slot = c.slots;
        if (c.framing != Framing::pcm_short) {
            cfg.slot_cfg.ws_width = sample_width_bits(c.slot_bits);
            cfg.slot_cfg.bit_shift = c.framing == Framing::philips;
        }
        if (c.slot_bits == SampleWidth::bits24)
            cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
        cfg.gpio_cfg.mclk = pad(c.mclk);
        cfg.gpio_cfg.bclk = pad(c.bclk);
        cfg.gpio_cfg.ws = pad(c.ws);
        cfg.gpio_cfg.dout = pad(c.dout);
        cfg.gpio_cfg.din = pad(c.din);
        tune_slot(cfg.slot_cfg, c.tuning);
        tune_clock(cfg.clk_cfg, c.tuning);
        invert_pins(cfg.gpio_cfg, c.tuning);
        return i2s_channel_init_tdm_mode(channel, &cfg);
    }
#endif
    if (c.mode == Mode::pdm) {
#if SOC_I2S_SUPPORTS_PDM_TX
        if (tx) {
            i2s_pdm_tx_config_t cfg{};
            cfg.clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG(c.sample_rate);
            if (c.pdm_pcm)
                cfg.slot_cfg = I2S_PDM_TX_SLOT_PCM_FMT_DEFAULT_CONFIG(width, slots);
            else
                cfg.slot_cfg = I2S_PDM_TX_SLOT_RAW_FMT_DEFAULT_CONFIG(width, slots);
            cfg.slot_cfg.data_fmt = c.pdm_pcm ? I2S_PDM_DATA_FMT_PCM : I2S_PDM_DATA_FMT_RAW;
            cfg.gpio_cfg.clk = pad(pdm_clock_pin(c));
            cfg.gpio_cfg.dout = pad(c.dout);
            cfg.gpio_cfg.invert_flags.clk_inv = c.tuning.ws_inv;
            tune_clock(cfg.clk_cfg, c.tuning);
            return i2s_channel_init_pdm_tx_mode(channel, &cfg);
        }
#endif
#if SOC_I2S_SUPPORTS_PDM_RX
        if (!tx) {
            i2s_pdm_rx_config_t cfg{};
            cfg.clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(c.sample_rate);
            if (c.pdm_pcm)
                cfg.slot_cfg = I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(width, slots);
            else
                cfg.slot_cfg = I2S_PDM_RX_SLOT_RAW_FMT_DEFAULT_CONFIG(width, slots);
            cfg.slot_cfg.data_fmt = c.pdm_pcm ? I2S_PDM_DATA_FMT_PCM : I2S_PDM_DATA_FMT_RAW;
            cfg.slot_cfg.slot_mask = static_cast<i2s_pdm_slot_mask_t>(c.slot_mask);
            cfg.gpio_cfg.clk = pad(pdm_clock_pin(c));
            cfg.gpio_cfg.din = pad(c.din);
            cfg.gpio_cfg.invert_flags.clk_inv = c.tuning.ws_inv;
            tune_clock(cfg.clk_cfg, c.tuning);
            return i2s_channel_init_pdm_rx_mode(channel, &cfg);
        }
#endif
    }
    return ESP_ERR_NOT_SUPPORTED;
}
} // namespace
#endif

Capabilities capabilities() {
    Capabilities c{};
#if SOC_I2S_SUPPORTED
    c.controllers = SOC_I2S_NUM;
    c.modes = mode_mask(Mode::standard);
    c.tx = c.rx = c.duplex = true;
    c.tuning = true;
#if SOC_I2S_SUPPORTS_APLL
    c.apll = true;
#endif
#if SOC_I2S_HW_VERSION_2
    c.serial_order = true;
#endif
#if SOC_I2S_SUPPORTS_TDM
    c.modes |= mode_mask(Mode::tdm);
#endif
#if SOC_I2S_SUPPORTS_PDM_TX || SOC_I2S_SUPPORTS_PDM_RX
    c.modes |= mode_mask(Mode::pdm);
#endif
#if SOC_I2S_SUPPORTS_PDM_TX
    c.pdm_tx = true;
#endif
#if SOC_I2S_SUPPORTS_PDM_RX
    c.pdm_rx = true;
#endif
#if SOC_I2S_SUPPORTS_PCM2PDM
    c.pdm_tx_pcm = true;
#endif
#if SOC_I2S_SUPPORTS_PDM2PCM
    c.pdm_rx_pcm = true;
#endif
#endif
    return c;
}

Result open(const Config& c, Port& port) {
#if SOC_I2S_SUPPORTED
    port = 0;
    if (c.controller >= SOC_I2S_NUM || c.controller < kNoController ||
        c.framing == Framing::right_justified || (c.role == Role::target && c.mclk.valid()))
        return Result::unsupported;
#if !SOC_I2S_HW_VERSION_1
    if (c.msb_right != MsbRight::automatic)
        return Result::unsupported;
#endif
    if (c.mode != Mode::standard && c.msb_right != MsbRight::automatic)
        return Result::unsupported;
    if (c.mode == Mode::pdm) {
        // PDM has no WS slot or BCLK; only the clock inversion (ws_inv) and APLL apply.
        const auto& t = c.tuning;
        if (t.ws_width || t.ws_pol || !t.left_align || t.big_endian || t.lsb_first || t.mclk_inv ||
            t.bclk_inv)
            return Result::unsupported;
        if (c.role != Role::controller || (c.din.valid() && c.dout.valid()) ||
            c.sample_bits != SampleWidth::bits16 || c.slot_bits != SampleWidth::bits16 ||
            (c.dout.valid() && c.slot_mask == kRightSlotMask))
            return Result::unsupported;
#if !SOC_I2S_SUPPORTS_PDM_TX
        if (c.dout.valid())
            return Result::unsupported;
#endif
#if !SOC_I2S_SUPPORTS_PDM_RX
        if (c.din.valid())
            return Result::unsupported;
#endif
        if (c.pdm_pcm && ((c.din.valid() && !capabilities().pdm_rx_pcm) ||
                             (c.dout.valid() && !capabilities().pdm_tx_pcm)))
            return Result::unsupported;
        if (c.pdm_pcm && c.controller > kFirstController)
            return Result::unsupported;
    }
    std::size_t index{};
    while (index < states.size() && states[index].occupied)
        ++index;
    if (index == states.size())
        return Result::busy;
    auto& s = states[index];
    s.config = c;
    s.overruns = 0;
    s.underruns = 0;
    s.received = 0;
    s.transmitted = 0;
    s.sent_pages = 0;
    s.written = false;
    s.swap_mono = false;
    s.packed24 = c.sample_bits == SampleWidth::bits24;
#if CONFIG_IDF_TARGET_ESP32
    s.swap_mono = c.mode == Mode::standard && c.sample_bits == SampleWidth::bits16 &&
                  channel_count(c) == kMonoChannelCount;
    s.packed24 = false;
#endif
    s.tx_odd = false;
    s.tx_size = s.tx_offset = s.rx_size = s.rx_offset = 0;
    s.occupied = true;
    port = static_cast<Port>(index);
    using NativeId = decltype(i2s_chan_config_t{}.id);
    const auto role = c.role == Role::controller ? I2S_ROLE_MASTER : I2S_ROLE_SLAVE;
    i2s_chan_config_t cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(static_cast<NativeId>(kFirstController), role);
    cfg.dma_desc_num = c.tuning.dma_descriptors;
    cfg.dma_frame_num = c.dma_frames;
    if (s.swap_mono)
        cfg.dma_frame_num =
            (cfg.dma_frame_num + kPcm16DmaFrameAlignment - 1) & ~(kPcm16DmaFrameAlignment - 1);
    if (c.sample_bits == SampleWidth::bits24)
        cfg.dma_frame_num =
            ((cfg.dma_frame_num + kPcm24DmaFrameAlignment - 1) / kPcm24DmaFrameAlignment) *
            kPcm24DmaFrameAlignment;
    if (cfg.dma_frame_num * frame_bytes(c) > kMaximumDmaPayloadBytes) {
        s.occupied = false;
        return Result::invalid_argument;
    }
    cfg.auto_clear_after_cb = true;
    auto err = ESP_ERR_NOT_FOUND;
    s.controller = kNoController;
    for (unsigned candidate = 0; candidate < SOC_I2S_NUM; ++candidate) {
        if (c.controller >= 0 && candidate != unsigned(c.controller))
            continue;
        // The exposed PCM/PDM converters belong to controller zero. Raw PDM
        // remains independently selectable on other SDK-supported controllers.
        if (c.mode == Mode::pdm && c.pdm_pcm && candidate != kFirstController)
            continue;
        bool conflict{};
        for (const auto& other : states)
            if (&other != &s && other.occupied && other.controller == int(candidate) &&
                (other.config.mclk.valid() || c.mclk.valid()))
                conflict = true;
        // A controller exposes one external MCLK binding. PDM initialization
        // also rebinds it internally, even with no MCLK pad. Retain the complete
        // controller for an external MCLK lease instead of changing its owner.
        if (conflict)
            continue;
        cfg.id = static_cast<NativeId>(candidate);
        err = i2s_new_channel(
            &cfg, c.dout.valid() ? &s.tx : nullptr, c.din.valid() ? &s.rx : nullptr);
        if (err == ESP_OK) {
            s.controller = candidate;
            break;
        }
        if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_INVALID_STATE)
            break;
    }
    // Initialize TX first: both IDF 5.5 and 6.1 make the later initialized
    // compatible RX channel the internal slave. IDF 6.1 also checks its clock ratio.
    if (err == ESP_OK && s.tx)
        err = initialize(s.tx, c, true);
    if (err == ESP_OK && s.rx)
        err = initialize(s.rx, c, false);
    if (err == ESP_OK && s.rx) {
        i2s_event_callbacks_t callbacks{};
        callbacks.on_recv_q_ovf = overflow;
        err = i2s_channel_register_event_callback(s.rx, &callbacks, &s);
    }
    if (err == ESP_OK && s.tx) {
        i2s_event_callbacks_t callbacks{};
        callbacks.on_send_q_ovf = starvation;
        callbacks.on_sent = sent;
        err = i2s_channel_register_event_callback(s.tx, &callbacks, &s);
    }
    if (err == ESP_OK && s.rx) {
        err = i2s_channel_enable(s.rx);
        s.rx_enabled = err == ESP_OK;
    }
    if (err == ESP_OK && s.tx) {
        err = i2s_channel_enable(s.tx);
        s.tx_enabled = err == ESP_OK;
    }
    if (err == ESP_OK)
        return Result::success;
    const auto result = translate(err);
    if (!release(s))
        return Result::failed;
    s.occupied = false;
    // Native peripheral errors with successful rollback do not quarantine pads.
    return result == Result::failed ? Result::unsupported : result;
#else
    (void)c;
    (void)port;
    return Result::unsupported;
#endif
}

Result close(Port port) {
#if SOC_I2S_SUPPORTED
    if (port >= states.size() || !states[port].occupied)
        return Result::invalid_argument;
    auto& s = states[port];
    if (!release(s))
        return Result::failed;
    for (auto p : {s.config.mode == Mode::pdm ? pdm_clock_pin(s.config) : s.config.bclk,
             s.config.mode == Mode::pdm ? PinId{} : s.config.ws, s.config.dout, s.config.din,
             s.config.mode == Mode::pdm ? PinId{} : s.config.mclk})
        if (p.valid() && gpio_configure_input(p, GpioPull::none) != GpioResult::success)
            return Result::failed;
    s.occupied = false;
    return Result::success;
#else
    (void)port;
    return Result::unsupported;
#endif
}

bool tx_idle(Port port) {
#if SOC_I2S_SUPPORTED
    if (port >= states.size() || !states[port].occupied)
        return false;
    auto& s = states[port];
    if ((s.swap_mono || s.packed24) && s.tx) {
        if (pump(s) != ESP_OK || s.tx_offset != s.tx_size)
            return false;
        if (s.tx_odd) {
            // A drain commits an odd final sample with one trailing silence
            // sample so the hardware's two-sample exchange remains defined.
            std::fill_n(s.tx_cache, kMono16SampleBytes, 0);
            std::memcpy(s.tx_cache + kMono16SampleBytes, s.tx_one, kMono16SampleBytes);
            s.tx_size = kMono16DmaGroupBytes;
            s.tx_offset = 0;
            s.tx_odd = false;
            pump(s);
            return false;
        }
    }
    // The SDK's write fills completed cyclic DMA pages. One complete descriptor
    // cycle plus the current page ensures every last accepted sample was sent.
    return !s.tx || !s.written ||
           std::uint32_t(s.sent_pages.load(std::memory_order_relaxed) - s.written_epoch) >=
               std::uint32_t{s.config.tuning.dma_descriptors} + 1U;
#else
    (void)port;
    return true;
#endif
}

Transfer read(Port port, std::uint8_t* data, std::size_t size) {
#if SOC_I2S_SUPPORTED
    if (port >= states.size() || !states[port].occupied || !states[port].rx)
        return {Result::invalid_argument, 0};
    auto& s = states[port];
    if (s.swap_mono || s.packed24) {
        std::size_t count{};
        auto err = ESP_OK;
        while (count < size) {
            if (s.rx_offset == s.rx_size) {
                s.rx_offset = s.rx_size = 0;
                const auto channels = channel_count(s.config);
                const auto native_size = s.packed24 ? (kPcm24SamplesPerBatch / channels) *
                                                          channels * kPcm24PackedBytesPerSample
                                                    : sizeof(s.rx_cache);
                err = i2s_channel_read(
                    s.rx, s.rx_cache, native_size, &s.rx_size, kNonblockingTimeoutTicks);
                if (s.rx_size % (s.packed24 ? channels * kPcm24PackedBytesPerSample
                                            : kPcm24StorageBytesPerSample))
                    return {Result::failed, count};
                if (!s.rx_size)
                    break;
                if (s.packed24) {
                    const auto samples = s.rx_size / kPcm24PackedBytesPerSample;
                    for (auto n = samples; n; --n) {
                        std::memmove(s.rx_cache + (n - 1) * kPcm24StorageBytesPerSample +
                                         kPcm24LeadingPaddingBytes,
                            s.rx_cache + (n - 1) * kPcm24PackedBytesPerSample,
                            kPcm24PackedBytesPerSample);
                        s.rx_cache[(n - 1) * kPcm24StorageBytesPerSample] = 0;
                    }
                    s.rx_size = samples * kPcm24StorageBytesPerSample;
                } else
                    exchange_samples(s.rx_cache, s.rx_size);
            }
            const auto chunk = std::min(size - count, s.rx_size - s.rx_offset);
            std::memcpy(data + count, s.rx_cache + s.rx_offset, chunk);
            s.rx_offset += chunk;
            count += chunk;
        }
        s.received += count;
        return {err == ESP_ERR_TIMEOUT ? Result::success : translate(err), count};
    }
    std::size_t count{};
    const auto err = i2s_channel_read(s.rx, data, size, &count, kNonblockingTimeoutTicks);
    s.received += count;
    return {err == ESP_ERR_TIMEOUT ? Result::success : translate(err), count};
#else
    (void)port;
    (void)data;
    (void)size;
    return {Result::unsupported, 0};
#endif
}

Transfer write(Port port, const std::uint8_t* data, std::size_t size) {
#if SOC_I2S_SUPPORTED
    if (port >= states.size() || !states[port].occupied || !states[port].tx)
        return {Result::invalid_argument, 0};
    auto& s = states[port];
    if (s.packed24) {
        auto err = pump(s);
        if (err != ESP_OK)
            return {translate(err), 0};
        if (s.tx_offset != s.tx_size)
            return {};
        const auto channels = channel_count(s.config);
        const auto samples = std::min<std::size_t>(size / (channels * kPcm24StorageBytesPerSample),
                                 kPcm24SamplesPerBatch / channels) *
                             channels;
        for (std::size_t n = 0; n < samples; ++n)
            std::memcpy(s.tx_cache + n * kPcm24PackedBytesPerSample,
                data + n * kPcm24StorageBytesPerSample + kPcm24LeadingPaddingBytes,
                kPcm24PackedBytesPerSample);
        s.tx_size = samples * kPcm24PackedBytesPerSample;
        s.tx_offset = 0;
        err = pump(s);
        s.transmitted += samples * kPcm24StorageBytesPerSample;
        return {translate(err), samples * kPcm24StorageBytesPerSample};
    }
    if (s.swap_mono) {
        auto err = pump(s);
        if (err != ESP_OK)
            return {translate(err), 0};
        if (s.tx_offset != s.tx_size)
            return {};
        const auto base = s.tx_odd ? kMono16SampleBytes : 0U;
        if (s.tx_odd)
            std::memcpy(s.tx_cache, s.tx_one, kMono16SampleBytes);
        const auto count = std::min(size, sizeof(s.tx_cache) - base);
        std::memcpy(s.tx_cache + base, data, count);
        const auto total = base + count;
        s.tx_odd = total % kMono16DmaGroupBytes != 0;
        if (s.tx_odd)
            std::memcpy(s.tx_one, s.tx_cache + total - kMono16SampleBytes, kMono16SampleBytes);
        s.tx_size = total & ~(kMono16DmaGroupBytes - 1U);
        s.tx_offset = 0;
        exchange_samples(s.tx_cache, s.tx_size);
        err = pump(s);
        s.transmitted += count;
        return {translate(err), count};
    }
    std::size_t count{};
    const auto err = send(s, data, size, count);
    s.transmitted += count;
    return {err == ESP_ERR_TIMEOUT ? Result::success : translate(err), count};
#else
    (void)port;
    (void)data;
    (void)size;
    return {Result::unsupported, 0};
#endif
}

Status status(Port port) {
#if SOC_I2S_SUPPORTED
    if (port < states.size() && states[port].occupied) {
        const auto& s = states[port];
        return {s.received.load(std::memory_order_relaxed),
            s.transmitted.load(std::memory_order_relaxed),
            s.overruns.load(std::memory_order_relaxed), s.underruns.load(std::memory_order_relaxed),
            s.config.sample_rate};
    }
#else
    (void)port;
#endif
    return {};
}

void poll() {
#if SOC_I2S_SUPPORTED
    for (auto& s : states)
        if (s.occupied && s.tx_enabled && (s.swap_mono || s.packed24))
            pump(s);
#endif
}
} // namespace newmota::platform::i2s
