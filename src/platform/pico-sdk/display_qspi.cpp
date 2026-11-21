/*
  display_qspi.cpp - Pico PIO and DMA four-lane display transport

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file display_qspi.cpp
 * @brief Mode-0 single-lane headers and four-lane pixel data on one PIO machine.
 */
#include "newmota/platform/display/native.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/pio_instructions.h"
#include "pico/time.h"
#include <array>

namespace newmota::platform::display::detail {
namespace {
PIO port{};
int machine{-1}, channel{-1};
unsigned offset{}, clock_pin{}, data_pin{}, select_pin{};
float divider{};
/** @brief Retained instruction slots shared by single-lane and quad phases.
 * OUT stalls with SCK LOW after the preceding HIGH half-cycle when autopull drains.
 */
const std::array<std::uint16_t, 2> instructions{
    {static_cast<std::uint16_t>(pio_encode_out(pio_pins, 1) | pio_encode_sideset(1, 0)),
        static_cast<std::uint16_t>(pio_encode_nop() | pio_encode_sideset(1, 1))}};
const pio_program program{instructions.data(), 2, -1};

/** @brief Drains DMA, FIFO and shifter, with a fresh terminal stall before CS rises. */
Result phase(const std::uint8_t* bytes, std::size_t length, bool quad) {
    if (!length)
        return Result::success;
    pio_sm_set_enabled(port, machine, false);
    port->instr_mem[offset] = pio_encode_out(pio_pins, quad ? 4 : 1) | pio_encode_sideset(1, 0);
    auto config = pio_get_default_sm_config();
    sm_config_set_wrap(&config, offset, offset + 1);
    sm_config_set_sideset(&config, 1, false, false);
    sm_config_set_sideset_pins(&config, clock_pin);
    sm_config_set_out_pins(&config, data_pin, quad ? 4 : 1);
    sm_config_set_out_shift(&config, false, true, 8);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&config, divider);
    pio_sm_init(port, machine, offset, &config);
    auto dma_config = dma_channel_get_default_config(channel);
    // Narrow writes replicate each byte into every FIFO byte lane. Shift-left
    // and autopull=8 therefore emit its high bit/nibble first on both RP chips.
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_8);
    channel_config_set_read_increment(&dma_config, true);
    channel_config_set_write_increment(&dma_config, false);
    channel_config_set_dreq(&dma_config, pio_get_dreq(port, machine, true));
    dma_channel_configure(channel, &dma_config, &port->txf[machine], bytes, length, true);
    pio_sm_set_enabled(port, machine, true);
    const auto deadline = time_us_64() + 100000;
    const auto stall = 1U << (PIO_FDEBUG_TXSTALL_LSB + machine);
    bool draining{};
    while (true) {
        if (!dma_channel_is_busy(channel) && pio_sm_is_tx_fifo_empty(port, machine)) {
            if (!draining) {
                port->fdebug = stall;
                draining = true;
            } else if (port->fdebug & stall)
                return Result::success;
        }
        if (time_us_64() >= deadline) {
            dma_channel_abort(channel);
            pio_sm_set_enabled(port, machine, false);
            return Result::timeout;
        }
    }
}
} // namespace

Result native_qspi_preflight(const Config& c) {
    if (c.spi_bus != 1 || c.clock < 0 || c.clock >= 32 || c.cs < 0 || c.cs >= 32 ||
        c.quad_pins[0] < 0 || c.quad_pins[0] > 28 ||
        static_cast<unsigned>(c.speed) * 2000000U > clock_get_hz(clk_sys))
        return Result::unsupported;
    for (unsigned i = 1; i < 4; ++i)
        if (c.quad_pins[i] != c.quad_pins[0] + static_cast<int>(i))
            return Result::unsupported;
    return Result::success;
}

Result native_qspi_open(const Config& c) {
    for (unsigned i = 0; i < NUM_PIOS; ++i) {
        auto candidate = pio_get_instance(i);
        if (!pio_can_add_program(candidate, &program))
            continue;
        machine = pio_claim_unused_sm(candidate, false);
        if (machine >= 0) {
            port = candidate;
            break;
        }
    }
    if (!port)
        return Result::busy;
    channel = dma_claim_unused_channel(false);
    if (channel < 0) {
        pio_sm_unclaim(port, machine);
        port = nullptr;
        return Result::busy;
    }
    offset = pio_add_program(port, &program);
    clock_pin = c.clock;
    data_pin = c.quad_pins[0];
    select_pin = c.cs;
    divider = static_cast<float>(clock_get_hz(clk_sys)) / (c.speed * 2000000.0f);
    gpio_init(select_pin);
    gpio_put(select_pin, true);
    gpio_set_dir(select_pin, GPIO_OUT);
    pio_gpio_init(port, clock_pin);
    pio_sm_set_consecutive_pindirs(port, machine, clock_pin, 1, true);
    for (auto pad : c.quad_pins)
        pio_gpio_init(port, pad);
    pio_sm_set_consecutive_pindirs(port, machine, data_pin, 4, true);
    pio_sm_set_pins_with_mask(port, machine, 0, (15U << data_pin) | (1U << clock_pin));
    return Result::success;
}

Result native_qspi_write(
    bool pixels, std::uint8_t opcode, const std::uint8_t* bytes, std::size_t length) {
    if (!port || length > 4096 || (length && !bytes))
        return Result::failed;
    const std::uint8_t header[]{static_cast<std::uint8_t>(pixels ? 0x32 : 0x02), 0, opcode, 0};
    gpio_put(select_pin, false);
    auto result = phase(header, sizeof(header), false);
    if (result == Result::success)
        result = phase(bytes, length, pixels);
    gpio_put(select_pin, true);
    return result;
}
} // namespace newmota::platform::display::detail
