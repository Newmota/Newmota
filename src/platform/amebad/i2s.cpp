/*
  i2s.cpp - AmebaD native I2S DMA transport

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file i2s.cpp
 * @brief RTL8720 I2S page DMA with bounded capture and playback queues.
 */
#include "newmota/platform/i2s.h"
#include "newmota/platform/gpio.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
extern "C" {
#include "ameba_soc.h"
}

namespace newmota::platform::i2s {
namespace {
constexpr unsigned kDmaPageCount = 4;
constexpr std::size_t kDmaWordBytes = sizeof(std::uint32_t);
constexpr std::size_t kDmaBufferAlignmentBytes = 32;
constexpr std::size_t kMaximumDmaFrameBytes = kStandardSlotCount * sizeof(std::uint32_t);
constexpr std::size_t kFifoWordCount = 64;
constexpr std::uint32_t kDmaErrorInterruptMask = 0x1f0U;
constexpr std::uint32_t kDmaPageInterruptMask = 0x1ffU;
constexpr std::uint32_t kNoInterruptMask = 0;
constexpr std::uint32_t kInterruptVectorIndex = 0;
constexpr std::uint32_t kInterruptPriority = 5;
constexpr std::uint8_t kAmebaControllerCount = 1;
constexpr std::int8_t kAnyController = -1;
constexpr unsigned kPcm24AlignmentShift = 8;
constexpr std::array<std::uint16_t, 3> kBclkRoutePins{2, 52, 61};
constexpr std::array<std::uint16_t, 3> kWsRoutePins{4, 53, 63};
constexpr std::array<std::uint16_t, 2> kDoutRoutePins{51, 58};
constexpr std::array<std::uint16_t, 2> kDinRoutePins{0, 54};
constexpr std::array<std::uint16_t, 2> kMclkRoutePins{12, 55};
static_assert((kDmaBufferAlignmentBytes & (kDmaBufferAlignmentBytes - 1)) == 0,
    "DMA buffer alignment must be a power of two");
// Pinned rtl8721d_i2s.h documents a 64 x 32-bit FIFO.
constexpr std::uint32_t kFifoBytes = kFifoWordCount * kDmaWordBytes;

/** @brief One application producer/ISR consumer queue, or its RX counterpart. */
struct Queue {
    std::uint8_t* bytes{};
    std::atomic<std::uint32_t> produced{0}, consumed{0};
};

struct State {
    bool active{}, stopped{};
    Config config{};
    std::uint8_t* allocation{};
    std::uint8_t *tx_pages{}, *rx_pages{};
    Queue tx{}, rx{};
    std::size_t page_bytes{}, logical_page_bytes{}, capacity{};
    unsigned tx_next{}, rx_next{};
    std::atomic<std::uint32_t> tx_counts[kDmaPageCount]{};
    std::atomic<std::uint32_t> silence_completed{kFifoBytes + kMaximumDmaFrameBytes};
    std::atomic<std::uint32_t> received{0}, transmitted{0}, overruns{0}, underruns{0};
};

State state;

/** @brief Pairs user-facing rates with the pinned SDK's explicit hardware selectors. */
struct NativeRate {
    std::uint32_t hertz;
    std::uint32_t sdk_code;
};

constexpr std::array<NativeRate, 19> kNativeRates{{
    {8000, I2S_SR_8KHZ},
    {12000, I2S_SR_12KHZ},
    {16000, I2S_SR_16KHZ},
    {24000, I2S_SR_24KHZ},
    {32000, I2S_SR_32KHZ},
    {48000, I2S_SR_48KHZ},
    {64000, I2S_SR_64KHZ},
    {96000, I2S_SR_96KHZ},
    {192000, I2S_SR_192KHZ},
    {384000, I2S_SR_384KHZ},
    {7350, I2S_SR_7p35KHZ},
    {11025, I2S_SR_11p025KHZ},
    {14700, I2S_SR_14p7KHZ},
    {22050, I2S_SR_22p05KHZ},
    {29400, I2S_SR_29p4KHZ},
    {44100, I2S_SR_44p1KHZ},
    {58800, I2S_SR_58p8KHZ},
    {88200, I2S_SR_88p2KHZ},
    {176400, I2S_SR_176p4KHZ},
}};

/** @brief Finds the SDK clock selector for a supported sample rate. */
bool native_rate(std::uint32_t hz, std::uint32_t& code) {
    for (const auto& rate : kNativeRates)
        if (rate.hertz == hz) {
            code = rate.sdk_code;
            return true;
        }
    return false;
}

/** @brief Copies a bounded queue span; publication occurs separately. */
void copy_out(const Queue& q, std::uint32_t offset, std::uint8_t* data, std::size_t size) {
    const auto pos = offset % state.capacity;
    const auto first = std::min<std::size_t>(size, state.capacity - pos);
    memcpy(data, q.bytes + pos, first);
    memcpy(data + first, q.bytes, size - first);
}

void copy_in(Queue& q, std::uint32_t offset, const std::uint8_t* data, std::size_t size) {
    const auto pos = offset % state.capacity;
    const auto first = std::min<std::size_t>(size, state.capacity - pos);
    memcpy(q.bytes + pos, data, first);
    memcpy(q.bytes, data + first, size - first);
}

/** @brief Translates native low 24-bit DMA words to the portable left-aligned form. */
void align_24(std::uint8_t* data, std::size_t size, bool capture) {
    if (state.config.sample_bits != SampleWidth::bits24)
        return;
    for (std::size_t n = 0; n < size; n += kDmaWordBytes) {
        std::uint32_t word;
        memcpy(&word, data + n, sizeof(word));
        word = capture ? word << kPcm24AlignmentShift : word >> kPcm24AlignmentShift;
        memcpy(data + n, &word, sizeof(word));
    }
}

/** @brief Preserves both wire slots while exposing only the selected memory slot. */
void compact_mono(std::uint8_t* data) {
    if (state.config.slot_mask == kStereoSlotMask)
        return;
    const auto sample = sample_storage_bytes(state.config.sample_bits);
    const auto selected = state.config.slot_mask == kRightSlotMask ? sample : 0;
    for (std::size_t n = 0; n < state.config.dma_frames; ++n) {
        std::uint32_t word{};
        memcpy(&word, data + n * sample * kStandardSlotCount + selected, sample);
        memcpy(data + n * sample, &word, sample);
    }
}

/** @brief Expands mono backwards so each source word survives until it is read. */
std::size_t expand_mono(std::uint8_t* data, std::size_t count) {
    if (state.config.slot_mask == kStereoSlotMask)
        return count;
    const auto sample = sample_storage_bytes(state.config.sample_bits);
    const auto selected = state.config.slot_mask == kRightSlotMask ? sample : 0;
    for (auto n = count / sample; n > 0; --n) {
        std::uint32_t word{};
        memcpy(&word, data + (n - 1) * sample, sample);
        memset(data + (n - 1) * sample * kStandardSlotCount, 0, sample * kStandardSlotCount);
        memcpy(data + (n - 1) * sample * kStandardSlotCount + selected, &word, sample);
    }
    return count * kStandardSlotCount;
}

/** @brief Recycles page DMA immediately; application work cannot stall capture. */
std::uint32_t interrupt(void*) {
    std::uint32_t tx{}, rx{};
    I2S_ISRGet(I2S_DEV, &tx, &rx);
    I2S_INTClear(I2S_DEV, tx, rx);
    if (!state.config.din.valid())
        rx = 0;
    if (!state.config.dout.valid())
        tx = 0;
    if (rx & kDmaErrorInterruptMask)
        state.overruns.fetch_add(1, std::memory_order_relaxed);
    if (tx & kDmaErrorInterruptMask)
        state.underruns.fetch_add(1, std::memory_order_relaxed);
    for (unsigned n = 0; n < kDmaPageCount && (rx & (1U << state.rx_next)); ++n) {
        const auto page = state.rx_next;
        auto* data = state.rx_pages + page * state.page_bytes;
        const auto produced = state.rx.produced.load(std::memory_order_relaxed);
        const auto consumed = state.rx.consumed.load(std::memory_order_acquire);
        if (state.capacity - (produced - consumed) >= state.logical_page_bytes) {
            compact_mono(data);
            align_24(data, state.logical_page_bytes, true);
            copy_in(state.rx, produced, data, state.logical_page_bytes);
            state.rx.produced.store(produced + state.logical_page_bytes, std::memory_order_release);
            state.received.fetch_add(state.logical_page_bytes, std::memory_order_relaxed);
        } else {
            state.overruns.fetch_add(1, std::memory_order_relaxed);
        }
        I2S_RxPageDMA_EN(I2S_DEV, page);
        state.rx_next = (page + 1) % kDmaPageCount;
    }
    for (unsigned n = 0; n < kDmaPageCount && (tx & (1U << state.tx_next)); ++n) {
        const auto page = state.tx_next;
        auto* data = state.tx_pages + page * state.page_bytes;
        const auto completed = state.tx_counts[page].load(std::memory_order_relaxed);
        state.transmitted.fetch_add(completed, std::memory_order_relaxed);
        const auto fence =
            kFifoBytes + kStandardSlotCount * sample_storage_bytes(state.config.sample_bits);
        const auto silence = state.silence_completed.load(std::memory_order_relaxed);
        state.silence_completed.store(
            completed ? 0 : std::min<std::uint32_t>(fence, silence + state.page_bytes),
            std::memory_order_release);
        const auto consumed = state.tx.consumed.load(std::memory_order_relaxed);
        const auto produced = state.tx.produced.load(std::memory_order_acquire);
        const auto count = std::min<std::size_t>(produced - consumed, state.logical_page_bytes);
        copy_out(state.tx, consumed, data, count);
        const auto native_count = expand_mono(data, count);
        memset(data + native_count, 0, state.page_bytes - native_count);
        align_24(data, state.page_bytes, false);
        state.tx.consumed.store(consumed + count, std::memory_order_release);
        state.tx_counts[page].store(count, std::memory_order_release);
        if (count < state.logical_page_bytes)
            state.underruns.fetch_add(1, std::memory_order_relaxed);
        I2S_TxPageDMA_EN(I2S_DEV, page);
        state.tx_next = (page + 1) % kDmaPageCount;
    }
    return 0;
}

/** @brief Native pin routes from the pinned SDK's QFN48/68/88 I2S examples. */
bool routes(const Config& c) {
    const auto supports = [](PinId pin, const auto& choices) {
        return std::find(choices.begin(), choices.end(), pin.value()) != choices.end();
    };
    if (!supports(c.bclk, kBclkRoutePins) || !supports(c.ws, kWsRoutePins))
        return false;
    if (c.dout.valid() && !supports(c.dout, kDoutRoutePins))
        return false;
    if (c.din.valid() && !supports(c.din, kDinRoutePins))
        return false;
    if (c.mclk.valid() && !supports(c.mclk, kMclkRoutePins))
        return false;
    for (auto pin : {c.bclk, c.ws, c.dout, c.din, c.mclk})
        if (pin.valid() && !gpio_available(pin))
            return false;
    return true;
}
} // namespace

/** @brief Describes the one native bidirectional standard I2S controller. */
Capabilities capabilities() {
    return {kAmebaControllerCount, mode_mask(Mode::standard), true, true, true, false, false};
}

/** @brief Acquires the native integrated DMA controller and complete clock domain. */
Result open(const Config& c, Port& port) {
    std::uint32_t native_rate_code{};
    if (!c.bclk.valid() || !c.ws.valid() || (!c.dout.valid() && !c.din.valid()) || !c.dma_frames ||
        c.dma_frames > kMaximumDmaFrames)
        return Result::invalid_argument;
    if (c.controller >= static_cast<std::int8_t>(kAmebaControllerCount) ||
        c.controller < kAnyController || c.mode != Mode::standard ||
        c.framing == Framing::pcm_short || !valid_sample_width(c.sample_bits) ||
        c.slot_bits !=
            (c.sample_bits == SampleWidth::bits16 ? SampleWidth::bits16 : SampleWidth::bits32) ||
        c.slots != kStandardSlotCount ||
        (c.slot_mask != kLeftSlotMask && c.slot_mask != kRightSlotMask &&
            c.slot_mask != kStereoSlotMask) ||
        !native_rate(c.sample_rate, native_rate_code) ||
        (c.role == Role::target && c.mclk.valid()) || !routes(c))
        return Result::unsupported;
    if (state.active || (I2S_DEV->IS_CTL & BIT_CTRL_CTLX_I2S_EN))
        return Result::busy;
    const auto logical_page_bytes = frame_bytes(c) * c.dma_frames;
    const auto page_bytes = kStandardSlotCount * sample_storage_bytes(c.sample_bits) * c.dma_frames;
    // Standard mode always clocks both slots; mono is compacted at the DMA boundary.
    if (page_bytes < kStandardSlotCount * sizeof(std::uint32_t) || (page_bytes % kDmaWordBytes))
        return Result::invalid_argument;
    // Power-of-two queue capacity preserves indexing across uint32_t counter wrap.
    std::size_t capacity = 1;
    while (capacity < logical_page_bytes * kDmaPageCount)
        capacity <<= 1U;
    const auto hardware_bytes = page_bytes * kDmaPageCount;
    const auto direction_bytes = hardware_bytes + capacity;
    const unsigned directions = unsigned(c.dout.valid()) + unsigned(c.din.valid());
    auto* allocation = static_cast<std::uint8_t*>(
        calloc(1, direction_bytes * directions + kDmaBufferAlignmentBytes - 1));
    if (!allocation)
        return Result::no_memory;
    auto* next = reinterpret_cast<std::uint8_t*>(
        (reinterpret_cast<std::uintptr_t>(allocation) + kDmaBufferAlignmentBytes - 1) &
        ~std::uintptr_t(kDmaBufferAlignmentBytes - 1));
    state.config = c;
    state.allocation = allocation;
    state.capacity = capacity;
    state.page_bytes = page_bytes;
    state.logical_page_bytes = logical_page_bytes;
    state.tx_pages = state.rx_pages = nullptr;
    if (c.dout.valid()) {
        state.tx_pages = next;
        state.tx.bytes = next + hardware_bytes;
        next += direction_bytes;
    }
    if (c.din.valid()) {
        state.rx_pages = next;
        state.rx.bytes = next + hardware_bytes;
    }
    state.tx.produced.store(0);
    state.tx.consumed.store(0);
    state.rx.produced.store(0);
    state.rx.consumed.store(0);
    state.received.store(0);
    state.transmitted.store(0);
    state.overruns.store(0);
    state.underruns.store(0);
    state.tx_next = state.rx_next = 0;
    for (auto& count : state.tx_counts)
        count.store(0, std::memory_order_relaxed);
    state.silence_completed.store(
        kFifoBytes + kStandardSlotCount * sizeof(std::uint32_t), std::memory_order_relaxed);
    state.stopped = false;
    PLL_I2S_Set(ENABLE);
    PLL_PCM_Set(ENABLE);
    RCC_PeriphClockCmd(APBPeriph_I2S0, APBPeriph_I2S0_CLOCK, ENABLE);
    I2S_InitTypeDef config{};
    I2S_StructInit(&config);
    config.I2S_SlaveMode = c.role == Role::controller ? I2S_MASTER_MODE : I2S_SLAVE_MODE;
    config.I2S_WordLen = c.sample_bits == SampleWidth::bits16   ? I2S_WL_16
                         : c.sample_bits == SampleWidth::bits24 ? I2S_WL_24
                                                                : I2S_WL_32;
    config.I2S_Justify = c.framing == Framing::philips          ? I2S_JY_I2S
                         : c.framing == Framing::left_justified ? I2S_JY_LEFT
                                                                : I2S_JY_RIGHT;
    // Preserve the SDK's native word order used by its short/int audio examples.
    config.I2S_EndianSwap = I2S_ES_BIG;
    config.I2S_ChNum = I2S_CH_STEREO;
    config.I2S_PageNum = kDmaPageCount;
    config.I2S_PageSize = page_bytes / kDmaWordBytes;
    config.I2S_Rate = native_rate_code;
    config.I2S_TRxAct = c.dout.valid() ? (c.din.valid() ? I2S_TXRX : I2S_ONLY_TX) : I2S_ONLY_RX;
    I2S_Init(I2S_DEV, &config);
    for (auto pin : {c.bclk, c.ws, c.dout, c.din, c.mclk})
        if (pin.valid()) {
            PAD_PullCtrl(pin.value(), GPIO_PuPd_NOPULL);
            Pinmux_Config(pin.value(), PINMUX_FUNCTION_I2S);
        }
    I2S_SetDMABuf(I2S_DEV, state.tx_pages, state.rx_pages);
    if (c.dout.valid())
        I2S_TxDmaCmd(I2S_DEV, ENABLE);
    state.active = true;
    InterruptRegister(reinterpret_cast<IRQ_FUN>(interrupt), I2S0_PCM0_IRQ, kInterruptVectorIndex,
        kInterruptPriority);
    I2S_INTConfig(I2S_DEV, c.dout.valid() ? kDmaPageInterruptMask : kNoInterruptMask,
        c.din.valid() ? kDmaPageInterruptMask : kNoInterruptMask);
    InterruptEn(I2S0_PCM0_IRQ, kInterruptPriority);
    I2S_Cmd(I2S_DEV, ENABLE);
    port = 0;
    return Result::success;
}

/** @brief Stops integrated DMA before releasing any memory or pin ownership. */
Result close(Port port) {
    if (port || !state.active)
        return Result::invalid_argument;
    InterruptDis(I2S0_PCM0_IRQ);
    I2S_INTConfig(I2S_DEV, kNoInterruptMask, kNoInterruptMask);
    I2S_Cmd(I2S_DEV, DISABLE);
    I2S_TxDmaCmd(I2S_DEV, DISABLE);
    I2S_RxDmaCmd(I2S_DEV, DISABLE);
    InterruptUnRegister(I2S0_PCM0_IRQ);
    state.stopped = true;
    bool clean = true;
    for (auto pin : {state.config.bclk, state.config.ws, state.config.dout, state.config.din,
             state.config.mclk})
        if (pin.valid() && gpio_configure_input(pin, GpioPull::none) != GpioResult::success)
            clean = false;
    if (!clean)
        return Result::failed;
    free(state.allocation);
    state.allocation = state.tx_pages = state.rx_pages = nullptr;
    state.tx.bytes = state.rx.bytes = nullptr;
    state.active = false;
    return Result::success;
}

/** @brief Copies available whole frames from the bounded capture queue. */
Transfer read(Port port, std::uint8_t* data, std::size_t size) {
    if (port || !state.active || !state.config.din.valid() || (!data && size))
        return {Result::invalid_argument, 0};
    if (state.stopped)
        return {Result::failed, 0};
    const auto frame = frame_bytes(state.config);
    const auto consumed = state.rx.consumed.load(std::memory_order_relaxed);
    const auto produced = state.rx.produced.load(std::memory_order_acquire);
    const auto count = std::min<std::size_t>(size - size % frame, produced - consumed);
    if (count)
        copy_out(state.rx, consumed, data, count);
    state.rx.consumed.store(consumed + count, std::memory_order_release);
    return {Result::success, count};
}

/** @brief Queues whole frames; the ISR supplies silence whenever output starves. */
Transfer write(Port port, const std::uint8_t* data, std::size_t size) {
    if (port || !state.active || !state.config.dout.valid() || (!data && size))
        return {Result::invalid_argument, 0};
    if (state.stopped)
        return {Result::failed, 0};
    const auto frame = frame_bytes(state.config);
    const auto produced = state.tx.produced.load(std::memory_order_relaxed);
    const auto consumed = state.tx.consumed.load(std::memory_order_acquire);
    const auto count =
        std::min<std::size_t>(size - size % frame, state.capacity - (produced - consumed));
    if (count)
        copy_in(state.tx, produced, data, count);
    state.tx.produced.store(produced + count, std::memory_order_release);
    return {Result::success, count};
}

/** @brief Reports drain only after every data-bearing page completes on the wire. */
bool tx_idle(Port port) {
    if (port || !state.active || state.stopped)
        return false;
    if (!state.config.dout.valid())
        return true;
    // Snapshot the queue and pages together: the ISR transfers ownership between
    // them, while hardware may complete further pages during this short section.
    // Pending completions conservatively keep the port busy until their ISR runs.
    InterruptDis(I2S0_PCM0_IRQ);
    bool idle = state.tx.produced.load(std::memory_order_acquire) ==
                state.tx.consumed.load(std::memory_order_acquire);
    for (const auto& count : state.tx_counts)
        idle = idle && count.load(std::memory_order_acquire) == 0;
    // DMA completion can precede serial output. More than one FIFO of completed
    // silence, plus a frame in the output shifter, fences the final audio frame.
    idle = idle &&
           state.silence_completed.load(std::memory_order_acquire) >=
               kFifoBytes + kStandardSlotCount * sample_storage_bytes(state.config.sample_bits);
    InterruptEn(I2S0_PCM0_IRQ, kInterruptPriority);
    return idle;
}

/** @brief Reports native queue and hardware loss counters. */
Status status(Port port) {
    if (port || !state.active)
        return {};
    return {state.received.load(), state.transmitted.load(), state.overruns.load(),
        state.underruns.load(), state.config.sample_rate};
}

/** @brief Integrated DMA pages are serviced entirely by the bounded native ISR. */
void poll() {}
} // namespace newmota::platform::i2s
