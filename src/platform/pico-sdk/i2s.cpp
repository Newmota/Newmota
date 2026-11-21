/*
  i2s.cpp - RP2040 and RP2350 PIO and DMA audio transport

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file i2s.cpp
 * @brief Continuous standard I2S with bounded DMA pages and native frame queues.
 */
#include "newmota/platform/i2s.h"
#include "newmota/platform/gpio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/pio_instructions.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace newmota::platform::i2s {
namespace {
constexpr std::size_t kDmaPageCount = 2;
constexpr std::size_t kAlignedPageAllocationCount = kDmaPageCount + 1;
constexpr std::size_t kRingPageCount = 8;
constexpr std::size_t kPioInstructionCount = 13;
constexpr std::size_t kMinimumPageBytes = 64;
constexpr std::size_t kNativeDmaWordBytes = sizeof(std::uint32_t);
constexpr unsigned kPioSideSetPinCount = 2;
constexpr unsigned kPioDataPinCount = 1;
constexpr std::uint32_t kPioOutputDelayCycles = 1;
constexpr std::uint32_t kPioCounterAdjustment = 2;
constexpr std::uint32_t kPioShiftRegisterBits = 32;
constexpr std::uint32_t kPioDividerFractionBits = 8;
constexpr std::uint32_t kPioCyclesPerI2sBit = 4;
constexpr std::uint32_t kPioDividerScale = 256;
constexpr std::uint32_t kPioDividerFractionMask = kPioDividerScale - 1;
constexpr std::uint32_t kPioDividerMinimum = kPioDividerScale;
constexpr std::uint32_t kPioDividerMaximum = 0x00ffffffU;
constexpr std::uint32_t kWsPinOffsetFromBclk = 1;
constexpr std::uint32_t kMaximumGpioPinCount = 32;
constexpr std::uint32_t kProgramFirstInstruction = 1;
constexpr std::uint32_t kProgramLeftLoopAddress = 1;
constexpr std::uint32_t kProgramRightLoopAddress = 7;
constexpr std::uint32_t kProgramWrapInstruction = kPioInstructionCount - 1;
constexpr std::uint32_t kFinalDmaDrainEpochs = 1;
constexpr std::int8_t kAnyController = -1;
constexpr int kUnavailableDmaChannel = -1;
constexpr int kUnclaimedStateMachine = -1;
constexpr int kPioAutoProgramOrigin = -1;
constexpr std::size_t kFirstDmaPage = 0;
constexpr std::size_t kSecondDmaPage = kDmaPageCount - 1;

enum class PioClockState : std::uint8_t {
    left_bclk_low = 0,
    left_bclk_high = 1,
    right_bclk_low = 2,
    right_bclk_high = 3,
};

/** @brief Single producer/consumer queue; counters publish only complete frames. */
struct Ring {
    std::uint8_t* data{};
    std::size_t size{};
    std::atomic<std::uint32_t> head{}, tail{};

    bool allocate(std::size_t capacity) {
        data = static_cast<std::uint8_t*>(std::calloc(capacity, 1));
        size = capacity;
        head = 0;
        tail = 0;
        return data;
    }

    std::size_t push(const std::uint8_t* source, std::size_t count) {
        const auto h = head.load(std::memory_order_relaxed);
        const auto t = tail.load(std::memory_order_acquire);
        count = std::min<std::size_t>(count, size - std::uint32_t(h - t));
        const auto start = h & (size - 1);
        const auto first = std::min<std::size_t>(count, size - start);
        std::memcpy(data + start, source, first);
        std::memcpy(data, source + first, count - first);
        head.store(h + count, std::memory_order_release);
        return count;
    }

    std::size_t pop(std::uint8_t* target, std::size_t count) {
        const auto t = tail.load(std::memory_order_relaxed);
        const auto h = head.load(std::memory_order_acquire);
        count = std::min(count, std::size_t(h - t));
        const auto start = t & (size - 1);
        const auto first = std::min<std::size_t>(count, size - start);
        std::memcpy(target, data + start, first);
        std::memcpy(target + first, data, count - first);
        tail.store(t + count, std::memory_order_release);
        return count;
    }

    void release() {
        std::free(data);
        data = nullptr;
        size = 0;
    }
};

struct Direction {
    int channels[kDmaPageCount]{kUnavailableDmaChannel, kUnavailableDmaChannel};
    void* allocation{};
    std::uint8_t* pages{};
    Ring ring{};
};

struct State {
    PIO pio{};
    int sm{kUnclaimedStateMachine};
    unsigned offset{};
    std::uint16_t instructions[kPioInstructionCount]{};
    pio_program program{instructions, kPioInstructionCount, kPioAutoProgramOrigin};
    Direction tx{}, rx{};
    Config config{};
    std::size_t page_bytes{}, logical_page_bytes{};
    std::atomic<std::uint32_t> overruns{}, underruns{};
    std::atomic<std::uint32_t> tx_counts[kDmaPageCount]{};
    std::atomic<std::uint32_t> tx_epoch{}, last_data_epoch{};
    std::uint32_t received{}, transmitted{}, actual_rate{};
    bool active{}, program_loaded{};
};

std::array<State, NUM_PIOS> states{};
bool irq_installed{};

/** @brief Exchanges the two 16-bit lanes between native frames and PIO MSB order. */
void exchange_lanes(std::uint8_t* data, std::size_t size) {
    constexpr auto kSampleLaneBytes = sizeof(std::uint16_t);
    for (std::size_t i = 0; i < size; i += kNativeDmaWordBytes) {
        std::swap(data[i], data[i + kSampleLaneBytes]);
        std::swap(data[i + 1], data[i + kSampleLaneBytes + 1]);
    }
}

/** @brief Packs one selected wire slot without changing the two-slot wire clock. */
void compact_mono(State& s, std::uint8_t* data) {
    if (channel_count(s.config) == kStereoChannelCount)
        return;
    const auto word = sample_storage_bytes(s.config.sample_bits);
    const auto chosen = s.config.slot_mask == kRightSlotMask ? word : 0U;
    for (std::size_t out = 0; out < s.logical_page_bytes; out += word)
        std::memmove(data + out, data + out * 2 + chosen, word);
}

/** @brief Expands a selected mono slot backwards in the native DMA page. */
void expand_mono(State& s, std::uint8_t* data) {
    if (channel_count(s.config) == kStereoChannelCount)
        return;
    const auto word = sample_storage_bytes(s.config.sample_bits);
    const auto chosen = s.config.slot_mask == kRightSlotMask ? word : 0U;
    for (auto in = s.logical_page_bytes; in; in -= word) {
        const auto out = (in - word) * 2;
        std::memmove(data + out + chosen, data + in - word, word);
        std::memset(data + out + (chosen ? 0U : word), 0, word);
    }
}

/** @brief Refills completed pages and reloads their counts for the next chain trigger. */
void dma_handler() {
    const auto pending = dma_hw->ints0;
    for (auto& s : states) {
        if (!s.active)
            continue;
        const auto page_transfer_count =
            dma_encode_transfer_count(static_cast<uint>(s.page_bytes / kNativeDmaWordBytes));
        for (unsigned page = 0; page < kDmaPageCount; ++page) {
            if (s.tx.channels[page] >= 0 && (pending & (1U << s.tx.channels[page]))) {
                dma_hw->ints0 = 1U << s.tx.channels[page];
                if (dma_channel_is_busy(s.tx.channels[page])) {
                    s.underruns.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                auto* buffer = s.tx.pages + page * s.page_bytes;
                const auto epoch = s.tx_epoch.fetch_add(1, std::memory_order_relaxed) + 1;
                if (s.tx_counts[page].load(std::memory_order_acquire))
                    s.last_data_epoch.store(epoch, std::memory_order_release);
                const auto count = s.tx.ring.pop(buffer, s.logical_page_bytes);
                s.tx_counts[page].store(count, std::memory_order_release);
                if (count != s.logical_page_bytes) {
                    std::memset(buffer + count, 0, s.logical_page_bytes - count);
                    s.underruns.fetch_add(1, std::memory_order_relaxed);
                }
                expand_mono(s, buffer);
                if (s.config.sample_bits == SampleWidth::bits16)
                    exchange_lanes(buffer, s.page_bytes);
                dma_channel_set_transfer_count(
                    static_cast<uint>(s.tx.channels[page]), page_transfer_count, false);
            }
            if (s.rx.channels[page] >= 0 && (pending & (1U << s.rx.channels[page]))) {
                dma_hw->ints0 = 1U << s.rx.channels[page];
                if (dma_channel_is_busy(s.rx.channels[page])) {
                    s.overruns.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                auto* buffer = s.rx.pages + page * s.page_bytes;
                if (s.config.sample_bits == SampleWidth::bits16)
                    exchange_lanes(buffer, s.page_bytes);
                compact_mono(s, buffer);
                if (s.rx.ring.push(buffer, s.logical_page_bytes) != s.logical_page_bytes)
                    s.overruns.fetch_add(1, std::memory_order_relaxed);
                dma_channel_set_transfer_count(
                    static_cast<uint>(s.rx.channels[page]), page_transfer_count, false);
            }
        }
    }
}

/** @brief Claims both DMA lanes and their bounded storage before enabling hardware. */
bool allocate(Direction& d, std::size_t page_bytes, std::size_t logical_page_bytes) {
    d.channels[kFirstDmaPage] = dma_claim_unused_channel(false);
    if (d.channels[kFirstDmaPage] < 0)
        return false;
    d.channels[kSecondDmaPage] = dma_claim_unused_channel(false);
    if (d.channels[kSecondDmaPage] < 0)
        return false;
    d.allocation = std::calloc(kAlignedPageAllocationCount * page_bytes, 1);
    if (!d.allocation)
        return false;
    const auto aligned =
        (reinterpret_cast<std::uintptr_t>(d.allocation) + page_bytes - 1) & ~(page_bytes - 1);
    d.pages = reinterpret_cast<std::uint8_t*>(aligned);
    return d.ring.allocate(kRingPageCount * logical_page_bytes);
}

void free_direction(Direction& d) {
    for (auto& ch : d.channels)
        if (ch >= 0) {
            dma_channel_unclaim(ch);
            ch = kUnavailableDmaChannel;
        }
    std::free(d.allocation);
    d.allocation = nullptr;
    d.pages = nullptr;
    d.ring.release();
}

/** @brief Stops chain triggering before aborting either DMA lane. */
void stop(Direction& d) {
    for (auto ch : d.channels)
        if (ch >= 0) {
            dma_channel_set_irq0_enabled(ch, false);
            auto cfg = dma_get_channel_config(ch);
            channel_config_set_enable(&cfg, false);
            channel_config_set_chain_to(&cfg, ch);
            dma_channel_set_config(ch, &cfg, false);
        }
    for (auto ch : d.channels)
        if (ch >= 0) {
            dma_channel_abort(ch);
            dma_hw->ints0 = 1U << ch;
        }
}

/** @brief Two chained lanes alternate pages; IRQ restores each completed lane's count. */
void configure_dma(State& s, Direction& d, bool tx) {
    unsigned ring_bits{};
    for (auto size = s.page_bytes; size > 1; size >>= 1)
        ++ring_bits;
    for (unsigned page = 0; page < kDmaPageCount; ++page) {
        const auto ch = d.channels[page];
        auto cfg = dma_channel_get_default_config(ch);
        channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
        channel_config_set_read_increment(&cfg, tx);
        channel_config_set_write_increment(&cfg, !tx);
        channel_config_set_ring(&cfg, !tx, ring_bits);
        channel_config_set_chain_to(&cfg, d.channels[kDmaPageCount - 1 - page]);
        channel_config_set_dreq(&cfg, pio_get_dreq(s.pio, s.sm, tx));
        auto* buffer = d.pages + page * s.page_bytes;
        if (tx)
            dma_channel_configure(ch, &cfg, &s.pio->txf[s.sm], buffer,
                dma_encode_transfer_count(s.page_bytes / kNativeDmaWordBytes), false);
        else
            dma_channel_configure(ch, &cfg, buffer, &s.pio->rxf[s.sm],
                dma_encode_transfer_count(s.page_bytes / kNativeDmaWordBytes), false);
        dma_hw->ints0 = 1U << ch;
        dma_channel_set_irq0_enabled(ch, true);
    }
}
} // namespace

Capabilities capabilities() {
    return {NUM_PIOS, mode_mask(Mode::standard), true, true, true, false, false};
}

Result open(const Config& c, Port& port) {
    if (c.msb_right != MsbRight::automatic || c.mode != Mode::standard ||
        c.framing != Framing::philips || c.role != Role::controller ||
        (c.slot_mask != kLeftSlotMask && c.slot_mask != kRightSlotMask &&
            c.slot_mask != kStereoSlotMask) ||
        c.slots != kStandardSlotCount || c.sample_bits != c.slot_bits ||
        (c.sample_bits != SampleWidth::bits16 && c.sample_bits != SampleWidth::bits32) ||
        c.ws.value() != c.bclk.value() + kWsPinOffsetFromBclk || c.mclk.valid() ||
        c.controller >= NUM_PIOS || c.controller < kAnyController || !c.sample_rate)
        return Result::unsupported;
    const auto bit_rate =
        c.sample_rate * sample_width_bits(c.slot_bits) * kStandardSlotCount * kPioCyclesPerI2sBit;
    const auto divider =
        (std::uint64_t(clock_get_hz(clk_sys)) * kPioDividerScale + bit_rate / 2) / bit_rate;
    if (divider < kPioDividerMinimum || divider > kPioDividerMaximum)
        return Result::unsupported;
    for (auto pin : {c.bclk, c.ws, c.dout, c.din})
        if (pin.valid() && (pin.value() >= kMaximumGpioPinCount || !gpio_available(pin)))
            return Result::unsupported;
    if (!irq_installed && irq_get_exclusive_handler(DMA_IRQ_0))
        return Result::busy;
    std::size_t slot{};
    while (slot < states.size() && states[slot].pio)
        ++slot;
    if (slot == states.size())
        return Result::busy;
    auto& s = states[slot];
    s.config = c;
    const auto sideset = [](PioClockState state) {
        return pio_encode_sideset(kPioSideSetPinCount, static_cast<unsigned>(state));
    };
    const auto low = sideset(PioClockState::left_bclk_low);
    const auto high = sideset(PioClockState::left_bclk_high);
    const auto low_right = sideset(PioClockState::right_bclk_low);
    const auto high_right = sideset(PioClockState::right_bclk_high);
    const auto out = c.dout.valid() ? pio_encode_out(pio_pins, kPioDataPinCount) : pio_encode_nop();
    const auto in = c.din.valid() ? pio_encode_in(pio_pins, kPioDataPinCount) : pio_encode_nop();
    const auto counter =
        pio_encode_set(pio_x, sample_width_bits(c.slot_bits) - kPioCounterAdjustment);
    // Each bit is four PIO cycles: two LOW and two HIGH. WS switches during
    // the previous slot's LSB, exactly one BCLK before the next slot's MSB.
    // SET at index 6 reloads right X; SET at index 12 reloads left X before
    // wrapping to index 1. Index 0 only initializes the very first left slot.
    const std::uint16_t program[] = {std::uint16_t(counter | low),
        std::uint16_t(out | low | pio_encode_delay(kPioOutputDelayCycles)),
        std::uint16_t(in | high),
        std::uint16_t(pio_encode_jmp_x_dec(kProgramLeftLoopAddress) | high),
        std::uint16_t(out | low_right | pio_encode_delay(kPioOutputDelayCycles)),
        std::uint16_t(in | high_right), std::uint16_t(counter | high_right),
        std::uint16_t(out | low_right | pio_encode_delay(kPioOutputDelayCycles)),
        std::uint16_t(in | high_right),
        std::uint16_t(pio_encode_jmp_x_dec(kProgramRightLoopAddress) | high_right),
        std::uint16_t(out | low | pio_encode_delay(kPioOutputDelayCycles)),
        std::uint16_t(in | high), std::uint16_t(counter | high)};
    std::memcpy(s.instructions, program, sizeof(program));
    for (unsigned p = 0; p < NUM_PIOS; ++p) {
        // Config controller selects a PIO block; auto searches all available blocks.
        if (c.controller >= 0 && p != unsigned(c.controller))
            continue;
        auto candidate = pio_get_instance(p);
        if (!pio_can_add_program(candidate, &s.program))
            continue;
        s.sm = pio_claim_unused_sm(candidate, false);
        if (s.sm >= 0) {
            s.pio = candidate;
            break;
        }
    }
    if (!s.pio)
        return Result::busy;
    port = static_cast<Port>(slot);
    s.page_bytes = kMinimumPageBytes;
    while (s.page_bytes < c.dma_frames * kStandardSlotCount * sample_storage_bytes(c.sample_bits))
        s.page_bytes <<= 1;
    s.logical_page_bytes = s.page_bytes * channel_count(c) / kStandardSlotCount;
    if ((c.dout.valid() && !allocate(s.tx, s.page_bytes, s.logical_page_bytes)) ||
        (c.din.valid() && !allocate(s.rx, s.page_bytes, s.logical_page_bytes))) {
        free_direction(s.tx);
        free_direction(s.rx);
        pio_sm_unclaim(s.pio, s.sm);
        s.pio = nullptr;
        return Result::no_memory;
    }
    s.offset = pio_add_program(s.pio, &s.program);
    s.program_loaded = true;
    auto cfg = pio_get_default_sm_config();
    sm_config_set_wrap(
        &cfg, s.offset + kProgramFirstInstruction, s.offset + kProgramWrapInstruction);
    sm_config_set_sideset(&cfg, kPioSideSetPinCount, false, false);
    sm_config_set_sideset_pins(&cfg, c.bclk.value());
    if (c.dout.valid())
        sm_config_set_out_pins(&cfg, c.dout.value(), kPioDataPinCount);
    if (c.din.valid())
        sm_config_set_in_pins(&cfg, c.din.value());
    sm_config_set_out_shift(&cfg, false, c.dout.valid(), kPioShiftRegisterBits);
    sm_config_set_in_shift(&cfg, false, c.din.valid(), kPioShiftRegisterBits);
    sm_config_set_clkdiv_int_frac(
        &cfg, divider >> kPioDividerFractionBits, divider & kPioDividerFractionMask);
    s.actual_rate =
        std::uint64_t(clock_get_hz(clk_sys)) * kPioDividerScale /
        (divider * sample_width_bits(c.slot_bits) * kStandardSlotCount * kPioCyclesPerI2sBit);
    for (auto pin : {c.bclk, c.ws, c.dout, c.din})
        if (pin.valid()) {
            pio_gpio_init(s.pio, pin.value());
            pio_sm_set_consecutive_pindirs(
                s.pio, s.sm, pin.value(), kPioDataPinCount, pin.value() != c.din.value());
        }
    pio_sm_init(s.pio, s.sm, s.offset, &cfg);
    s.overruns = 0;
    s.underruns = 0;
    s.received = s.transmitted = 0;
    for (auto& count : s.tx_counts)
        count = 0;
    s.tx_epoch = 0;
    s.last_data_epoch = 0;
    if (!irq_installed) {
        irq_add_shared_handler(
            DMA_IRQ_0, dma_handler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
        irq_set_enabled(DMA_IRQ_0, true);
        irq_installed = true;
    }
    if (c.dout.valid())
        configure_dma(s, s.tx, true);
    if (c.din.valid())
        configure_dma(s, s.rx, false);
    s.active = true;
    std::uint32_t mask{};
    if (c.dout.valid())
        mask |= 1U << s.tx.channels[kFirstDmaPage];
    if (c.din.valid())
        mask |= 1U << s.rx.channels[kFirstDmaPage];
    dma_start_channel_mask(mask);
    pio_sm_set_enabled(s.pio, s.sm, true);
    return Result::success;
}

Result close(Port port) {
    if (port >= states.size() || !states[port].pio)
        return Result::invalid_argument;
    auto& s = states[port];
    pio_sm_set_enabled(s.pio, s.sm, false);
    stop(s.tx);
    stop(s.rx);
    s.active = false;
    for (auto pin : {s.config.bclk, s.config.ws, s.config.dout, s.config.din})
        if (pin.valid() && gpio_configure_input(pin, GpioPull::none) != GpioResult::success)
            return Result::failed;
    free_direction(s.tx);
    free_direction(s.rx);
    if (s.program_loaded)
        pio_remove_program(s.pio, &s.program, s.offset);
    s.program_loaded = false;
    pio_sm_unclaim(s.pio, s.sm);
    s.pio = nullptr;
    s.sm = kUnclaimedStateMachine;
    bool any{};
    for (const auto& other : states)
        any |= other.active;
    if (!any && irq_installed) {
        irq_remove_handler(DMA_IRQ_0, dma_handler);
        irq_installed = false;
    }
    return Result::success;
}

bool tx_idle(Port port) {
    if (port >= states.size() || !states[port].active)
        return false;
    const auto& s = states[port];
    bool page_dma_idle = true;
    for (const auto& count : s.tx_counts)
        page_dma_idle = page_dma_idle && count.load(std::memory_order_acquire) == 0;
    return !s.config.dout.valid() ||
           (s.tx.ring.head.load(std::memory_order_acquire) ==
                   s.tx.ring.tail.load(std::memory_order_acquire) &&
               page_dma_idle &&
               (!s.transmitted ||
                   std::uint32_t(s.tx_epoch.load(std::memory_order_acquire) -
                                 s.last_data_epoch.load(std::memory_order_acquire)) >=
                       kFinalDmaDrainEpochs));
}

Transfer read(Port port, std::uint8_t* data, std::size_t size) {
    if (port >= states.size() || !states[port].active || !states[port].config.din.valid())
        return {Result::invalid_argument, 0};
    auto& s = states[port];
    const auto count = s.rx.ring.pop(data, size);
    s.received += count;
    return {Result::success, count};
}

Transfer write(Port port, const std::uint8_t* data, std::size_t size) {
    if (port >= states.size() || !states[port].active || !states[port].config.dout.valid())
        return {Result::invalid_argument, 0};
    auto& s = states[port];
    const auto count = s.tx.ring.push(data, size);
    s.transmitted += count;
    return {Result::success, count};
}

Status status(Port port) {
    if (port >= states.size() || !states[port].active)
        return {};
    const auto& s = states[port];
    return {s.received, s.transmitted, s.overruns.load(std::memory_order_relaxed),
        s.underruns.load(std::memory_order_relaxed), s.actual_rate};
}

void poll() {}
} // namespace newmota::platform::i2s
