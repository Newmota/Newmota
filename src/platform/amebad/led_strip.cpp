/*
  led_strip.cpp - AmebaD native LED strip adapter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file led_strip.cpp
 * @brief AmebaD native LED strip adapter.
 */

#include "newmota/platform/led_strip.h"
#include "newmota/platform/gpio.h"
#include <array>
#include <atomic>
extern "C" {
#include "ameba_soc.h"
}

namespace newmota::platform::led_strip {
namespace {
struct State {
    PinId pin{};
    GDMA_InitTypeDef dma{};
    bool active{}, sending{};
    std::size_t capacity{};
    std::atomic<unsigned> completed{0};
};

std::array<State, 2> states{};

u32 interrupt(void* context) {
    auto& s = *static_cast<State*>(context);
    const auto status = GDMA_ClearINT(s.dma.GDMA_Index, s.dma.GDMA_ChNum);
    GDMA_Cmd(s.dma.GDMA_Index, s.dma.GDMA_ChNum, DISABLE);
    s.completed.store((status & ErrType) ? 2 : 1, std::memory_order_release);
    return 0;
}
} // namespace

Result open(PinId pin, std::size_t pixels, Port& port) {
    if (!pixels || pixels > kMaximumPixels)
        return Result::invalid_argument;
    if (!pin.valid() || !gpio_available(pin))
        return Result::unsupported;
    // Native SSI route table: PA16/PB18 => SPI0; PA12/PB4 => SPI1.
    const auto pad = pin.value();
    if (pad == 16 || pad == 50)
        port = 0;
    else if (pad == 12 || pad == 36)
        port = 1;
    else
        return Result::unsupported;
    auto& s = states[port];
    if (s.active)
        return Result::busy;
    auto* device = SPI_DEV_TABLE[port].SPIx;
    // Refuse a native SSI already in use instead of reconfiguring its pads.
    if (device->SSIENR)
        return Result::busy;
    RCC_PeriphClockCmd(port ? APBPeriph_SPI1 : APBPeriph_SPI0,
        port ? APBPeriph_SPI1_CLOCK : APBPeriph_SPI0_CLOCK, ENABLE);
    SSI_InitTypeDef config{};
    SSI_StructInit(&config);
    config.SPI_Role = SSI_MASTER;
    config.SPI_ClockDivider = port ? 16 : 32; // SSI1=50 MHz, SSI0=100 MHz => 3.125 MHz.
    config.SPI_DataFrameSize = DFS_8_BITS;
    config.SPI_TransferMode = TMOD_TO;
    config.SPI_SclkPhase = SCPH_TOGGLES_AT_START;
    config.SPI_SclkPolarity = SCPOL_INACTIVE_IS_LOW;
    config.SPI_SlaveSelectEnable = 1;
    if (port == 0)
        SSI_SetRole(device, SSI_MASTER);
    SSI_Init(device, &config);
    Pinmux_Config(pad, PINMUX_FUNCTION_SPIM); // Do not mux SCLK/MISO/CS pads.
    s.capacity = wire_size(pixels, true);
    s.pin = pin;
    s.active = true;
    return Result::success;
}

Result readiness(Port port) {
    if (port >= states.size() || !states[port].active)
        return Result::busy;
    auto& s = states[port];
    if (!s.sending)
        return Result::success;
    const auto completed = s.completed.load(std::memory_order_acquire);
    // A terminal error is observable without releasing uncertain DMA resources.
    if (completed == 2)
        return Result::failed;
    if (completed != 1)
        return Result::busy;
    auto* device = SPI_DEV_TABLE[port].SPIx;
    if (device->TXFLR || SSI_Busy(device))
        return Result::busy;
    SSI_SetDmaEnable(device, DISABLE, BIT_SHIFT_DMACR_TDMAE);
    GDMA_ChnlFree(s.dma.GDMA_Index, s.dma.GDMA_ChNum);
    s.sending = false;
    return Result::success;
}

Result transmit(Port port, const std::uint8_t* data, std::size_t size) {
    if (port >= states.size() || !states[port].active || !data || !size ||
        size > states[port].capacity || (reinterpret_cast<std::uintptr_t>(data) & 31) || (size & 3))
        return Result::invalid_argument;
    auto& s = states[port];
    if (s.completed.load(std::memory_order_acquire) == 2)
        return Result::failed;
    if (const auto result = readiness(port); result != Result::success)
        return result;
    s.completed.store(0, std::memory_order_release);
    auto* device = SPI_DEV_TABLE[port].SPIx;
    SSI_SetDmaEnable(device, ENABLE, BIT_SHIFT_DMACR_TDMAE);
    if (!SSI_TXGDMA_Init(port, &s.dma, &s, interrupt, const_cast<std::uint8_t*>(data), size)) {
        SSI_SetDmaEnable(device, DISABLE, BIT_SHIFT_DMACR_TDMAE);
        return Result::busy;
    }
    s.sending = true;
    return Result::success;
}

Result close(Port port) {
    if (port >= states.size() || !states[port].active)
        return Result::invalid_argument;
    if (const auto result = readiness(port); result != Result::success)
        return result;
    auto& s = states[port];
    auto* device = SPI_DEV_TABLE[port].SPIx;
    SSI_SetDmaEnable(device, DISABLE, BIT_SHIFT_DMACR_TDMAE);
    SSI_Cmd(device, DISABLE);
    if (gpio_configure_input(s.pin, GpioPull::none) != GpioResult::success)
        return Result::failed;
    s.active = false;
    s.capacity = 0;
    s.pin = {};
    return Result::success;
}
} // namespace newmota::platform::led_strip
