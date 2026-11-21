/*
  led_strip.cpp - Pico SDK native LED strip adapter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file led_strip.cpp
 * @brief Pico SDK native LED strip adapter.
 */

#include "newmota/platform/led_strip.h"
#include "newmota/platform/gpio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/pio_instructions.h"
#include <array>

namespace newmota::platform::led_strip {
namespace {
const std::uint16_t instruction = pio_encode_out(pio_pins, 1);
const pio_program program{&instruction, 1, -1};

struct State {
    PIO pio{};
    int sm{-1}, dma{-1};
    unsigned offset{};
    PinId pin{};
    bool sending{}, draining{};
    std::size_t capacity{};
};

std::array<State, 4> states{};
} // namespace

Result open(PinId pin, std::size_t pixels, Port& port) {
    if (!pixels || pixels > kMaximumPixels)
        return Result::invalid_argument;
    if (!pin.valid() || pin.value() >= 32 || !gpio_available(pin))
        return Result::unsupported;
    std::size_t slot{};
    while (slot < states.size() && states[slot].pio)
        ++slot;
    if (slot == states.size())
        return Result::busy;
    PIO pio{};
    int sm{-1};
    for (unsigned index = 0; index < NUM_PIOS; ++index) {
        auto candidate = pio_get_instance(index);
        if (!pio_can_add_program(candidate, &program))
            continue;
        sm = pio_claim_unused_sm(candidate, false);
        if (sm >= 0) {
            pio = candidate;
            break;
        }
    }
    if (!pio)
        return Result::busy;
    const int dma = dma_claim_unused_channel(false);
    if (dma < 0) {
        pio_sm_unclaim(pio, sm);
        return Result::busy;
    }
    const auto offset = pio_add_program(pio, &program);
    if (offset < 0) {
        dma_channel_unclaim(dma);
        pio_sm_unclaim(pio, sm);
        return Result::busy;
    }
    auto config = pio_get_default_sm_config();
    sm_config_set_wrap(&config, offset, offset);
    sm_config_set_out_pins(&config, pin.value(), 1);
    sm_config_set_out_shift(&config, false, true, 8);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&config, static_cast<float>(clock_get_hz(clk_sys)) / 3200000.0f);
    pio_gpio_init(pio, pin.value());
    pio_sm_set_consecutive_pindirs(pio, sm, pin.value(), 1, true);
    pio_sm_init(pio, sm, offset, &config);
    pio_sm_set_pins_with_mask(pio, sm, 0, 1U << pin.value());
    pio_sm_set_enabled(pio, sm, true);
    states[slot] = {pio, sm, dma, static_cast<unsigned>(offset), pin, false};
    states[slot].capacity = wire_size(pixels, true);
    port = static_cast<Port>(slot);
    return Result::success;
}

Result readiness(Port port) {
    if (port >= states.size() || !states[port].pio)
        return Result::busy;
    auto& s = states[port];
    if (!s.sending)
        return Result::success;
    if (dma_channel_is_busy(s.dma) || !pio_sm_is_tx_fifo_empty(s.pio, s.sm))
        return Result::busy;
    // TXSTALL means the final OUT has completed and the next autopull is
    // waiting. DMA alone can finish while bytes remain in the output shifter.
    const auto stall = 1U << (PIO_FDEBUG_TXSTALL_LSB + s.sm);
    if (!s.draining) {
        // Initial startup or a temporary FIFO underrun can set TXSTALL too.
        // Require a new stall after DMA and the FIFO have drained.
        s.pio->fdebug = stall;
        s.draining = true;
        return Result::busy;
    }
    if (!(s.pio->fdebug & stall))
        return Result::busy;
    s.sending = false;
    return Result::success;
}

Result transmit(Port port, const std::uint8_t* data, std::size_t size) {
    if (port >= states.size() || !states[port].pio || !data || !size ||
        size > states[port].capacity)
        return Result::invalid_argument;
    if (const auto result = readiness(port); result != Result::success)
        return result;
    auto& s = states[port];
    // Remove the previous terminal stall before restarting. Every encoded
    // frame ends in zero; on terminal stall the driven level remains LOW.
    s.pio->fdebug = 1U << (PIO_FDEBUG_TXSTALL_LSB + s.sm);
    auto config = dma_channel_get_default_config(s.dma);
    // RP2040/RP2350 normal IO writes replicate an 8-bit DMA value across
    // all four FIFO byte lanes (their Narrow IO register writes sections).
    // With shift-left and autopull=8, OUT therefore emits the source byte's
    // MSB first, without needing a separate 32-bit staging buffer.
    channel_config_set_transfer_data_size(&config, DMA_SIZE_8);
    channel_config_set_read_increment(&config, true);
    channel_config_set_write_increment(&config, false);
    channel_config_set_dreq(&config, pio_get_dreq(s.pio, s.sm, true));
    s.sending = true;
    s.draining = false;
    dma_channel_configure(s.dma, &config, &s.pio->txf[s.sm], data, size, true);
    return Result::success;
}

Result close(Port port) {
    if (port >= states.size() || !states[port].pio)
        return Result::invalid_argument;
    if (const auto result = readiness(port); result != Result::success)
        return result;
    auto& s = states[port];
    pio_sm_set_enabled(s.pio, s.sm, false);
    if (gpio_configure_input(s.pin, GpioPull::none) != GpioResult::success)
        return Result::failed;
    dma_channel_unclaim(s.dma);
    pio_remove_program(s.pio, &program, s.offset);
    pio_sm_unclaim(s.pio, s.sm);
    s = {};
    return Result::success;
}
} // namespace newmota::platform::led_strip
