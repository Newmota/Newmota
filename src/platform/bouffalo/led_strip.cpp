/*
  led_strip.cpp - Bouffalo native LED strip adapter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file led_strip.cpp
 * @brief Bouffalo native LED strip adapter.
 */

#include "newmota/platform/led_strip.h"
#include "newmota/platform/gpio.h"
#include <cstdlib>
extern "C" {
#include "bflb_spi.h"
#include "bflb_dma.h"
#include "bflb_gpio.h"
#include "bflb_clock.h"
#include "hardware/spi_reg.h"
}

namespace newmota::platform::led_strip {
namespace {
bflb_device_s* spi{};
bflb_device_s* dma{};
// SDK divides transfers into 4064-word nodes (last node up to 4095).
// Its reload writes nodes before checking capacity, so reserve the full worst case.
constexpr std::size_t descriptor_count = (wire_size(kMaximumPixels, true) + 4063U) / 4064U;
bflb_dma_channel_lli_pool_s descriptors[descriptor_count]{};
std::uint32_t* words{};
PinId pin{};
bool active{}, sending{};
std::size_t capacity{};
} // namespace

Result open(PinId pad, std::size_t pixels, Port& port) {
    if (!pixels || pixels > kMaximumPixels)
        return Result::invalid_argument;
    // BL602's unswapped master MOSI routes are GPIO0/4/8/12/16/20.
    if (!pad.valid() || !gpio_available(pad) || pad.value() > 20 || pad.value() % 4 != 0)
        return Result::unsupported;
    if (active)
        return Result::busy;
    spi = bflb_device_get_by_name("spi0");
    dma = bflb_device_get_by_name("dma0_ch0");
    if (!spi || !dma)
        return Result::unsupported;
    if (bflb_dma_channel_isbusy(dma) || bflb_spi_isbusy(spi) ||
        (getreg32(spi->reg_base + SPI_CONFIG_OFFSET) & SPI_CR_SPI_M_EN))
        return Result::busy;
    bflb_spi_config_s config{};
    config.freq = 3200000;
    config.role = SPI_ROLE_MASTER;
    config.mode = SPI_MODE0;
    config.data_width = SPI_DATA_WIDTH_8BIT;
    config.bit_order = SPI_BIT_MSB;
    config.byte_order = SPI_BYTE_LSB;
    config.tx_fifo_threshold = 1;
    bflb_spi_init(spi, &config);
    // The default SDK config inserts an interframe delay. Continuous master
    // mode and zero interval are required for a continuous LED waveform.
    bflb_spi_feature_control(spi, SPI_CMD_SET_CS_INTERVAL, true);
    auto interval = getreg32(spi->reg_base + SPI_PRD_1_OFFSET);
    putreg32(interval & ~SPI_CR_SPI_PRD_I_MASK, spi->reg_base + SPI_PRD_1_OFFSET);
    // SDK GET_FREQ omits the +1 per divider phase; derive the actual rate.
    const auto period = getreg32(spi->reg_base + SPI_PRD_0_OFFSET);
    const auto divider = ((period & SPI_CR_SPI_PRD_D_PH_0_MASK) >> SPI_CR_SPI_PRD_D_PH_0_SHIFT) +
                         ((period & SPI_CR_SPI_PRD_D_PH_1_MASK) >> SPI_CR_SPI_PRD_D_PH_1_SHIFT) + 2;
    const auto rate = bflb_clk_get_peripheral_clock(BFLB_DEVICE_TYPE_SPI, spi->idx) / divider;
    if (rate < 3000000 || rate > 3250000) {
        bflb_spi_deinit(spi);
        return Result::unsupported;
    }
    words =
        static_cast<std::uint32_t*>(std::calloc(wire_size(pixels, true), sizeof(std::uint32_t)));
    if (!words) {
        bflb_spi_deinit(spi);
        return Result::no_memory;
    }
    bflb_dma_channel_config_s dma_config{};
    dma_config.direction = DMA_MEMORY_TO_PERIPH;
    dma_config.src_req = DMA_REQUEST_NONE;
    dma_config.dst_req = DMA_REQUEST_SPI0_TX;
    dma_config.src_addr_inc = DMA_ADDR_INCREMENT_ENABLE;
    dma_config.dst_addr_inc = DMA_ADDR_INCREMENT_DISABLE;
    dma_config.src_burst_count = DMA_BURST_INCR1;
    dma_config.dst_burst_count = DMA_BURST_INCR1;
    dma_config.src_width = DMA_DATA_WIDTH_32BIT;
    dma_config.dst_width = DMA_DATA_WIDTH_32BIT;
    bflb_dma_channel_init(dma, &dma_config);
    bflb_spi_link_txdma(spi, true);
    bflb_gpio_init(bflb_device_get_by_name("gpio"), pad.value(),
        GPIO_FUNC_SPI0 | GPIO_ALTERNATE | GPIO_FLOAT | GPIO_DRV_1);
    capacity = wire_size(pixels, true);
    pin = pad;
    active = true;
    port = 0;
    return Result::success;
}

Result readiness(Port port) {
    if (port != 0 || !active)
        return Result::busy;
    if (!sending)
        return Result::success;
    if (bflb_dma_channel_isbusy(dma) || bflb_spi_isbusy(spi))
        return Result::busy;
    // An idle bus alone can be an underrun; also require the TX FIFO empty.
    const auto fifo = getreg32(spi->reg_base + SPI_FIFO_CONFIG_1_OFFSET);
    if (((fifo & SPI_TX_FIFO_CNT_MASK) >> SPI_TX_FIFO_CNT_SHIFT) != SPI_FIFO_WORD_NUM_MAX)
        return Result::busy;
    sending = false;
    return Result::success;
}

Result transmit(Port port, const std::uint8_t* data, std::size_t size) {
    if (port != 0 || !active || !data || !size || size > capacity)
        return Result::invalid_argument;
    if (const auto result = readiness(port); result != Result::success)
        return result;
    // BL602 FIFO entries are fixed 32-bit words, even for 8-bit frames.
    // Expand each encoded byte into the low byte of one FIFO word. Mixed
    // 8/32-bit DMA widths would pack four bytes and lose three frame bytes.
    for (std::size_t i = 0; i < size; ++i)
        words[i] = data[i];
    bflb_dma_channel_lli_transfer_s transfer{};
    transfer.src_addr = reinterpret_cast<std::uintptr_t>(words);
    transfer.dst_addr = spi->reg_base + SPI_FIFO_WDATA_OFFSET;
    transfer.nbytes = size * sizeof(*words);
    const auto result =
        bflb_dma_channel_lli_reload(dma, descriptors, descriptor_count, &transfer, 1);
    if (result < 0)
        return Result::failed;
    sending = true;
    bflb_dma_channel_start(dma);
    return Result::success;
}

Result close(Port port) {
    if (port != 0 || !active)
        return Result::invalid_argument;
    if (const auto result = readiness(port); result != Result::success)
        return result;
    bflb_dma_channel_stop(dma);
    bflb_spi_link_txdma(spi, false);
    bflb_spi_deinit(spi);
    if (gpio_configure_input(pin, GpioPull::none) != GpioResult::success)
        return Result::failed;
    std::free(words);
    words = nullptr;
    active = false;
    capacity = 0;
    pin = {};
    return Result::success;
}
} // namespace newmota::platform::led_strip
