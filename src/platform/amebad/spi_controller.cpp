/*
  spi_controller.cpp - AmebaD SSI controller owner

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file spi_controller.cpp
 * @brief SSI0/SSI1 in full-duplex master mode, shared by devices with identical pins.
 *
 * SSI0 runs from 100 MHz and SSI1 from 50 MHz with an even divider of at least 2. Each
 * controller has two fixed pin sets. Framing changes with the controller disabled, as its
 * control and baud registers require.
 */
#include "newmota/platform/spi_controller.h"

#include "newmota/platform/gpio.h"
#include "newmota/platform/platform.h"
extern "C" {
#include "ameba_soc.h"
}

#include <array>

namespace newmota::platform::spi_controller {
namespace {

constexpr std::uint32_t kTimeoutMs = 100U;
constexpr std::uint32_t kInFlight = 32U; // Below the 64-entry RX FIFO.

struct Bus final {
    Wiring wiring{};
    Format format{};
    std::uint8_t users{};
};

std::array<Bus, 2> buses{};

[[nodiscard]] SPI_TypeDef* device(const std::uint8_t bus) {
    return SPI_DEV_TABLE[bus].SPIx;
}

[[nodiscard]] std::uint32_t input_hz(const std::uint8_t bus) {
    return bus != 0U ? 50000000U : 100000000U;
}

/** @brief SSI0 on PA16/18/17 or PB18/20/19, SSI1 on PA12/14/13 or PB4/6/5 (MOSI/SCLK/MISO). */
[[nodiscard]] bool route(const Wiring& wiring) {
    const auto pins = [&](const int mosi, const int clock, const int miso) {
        return wiring.mosi == mosi && wiring.clock == clock &&
               (wiring.miso < 0 || wiring.miso == miso);
    };
    return wiring.bus == 0U   ? pins(16, 18, 17) || pins(50, 52, 51)
           : wiring.bus == 1U ? pins(12, 14, 13) || pins(36, 38, 37)
                              : false;
}

[[nodiscard]] bool same(const Wiring& left, const Wiring& right) {
    return left.clock == right.clock && left.mosi == right.mosi && left.miso == right.miso;
}

[[nodiscard]] bool same(const Format& left, const Format& right) {
    return left.hz == right.hz && left.mode == right.mode && left.bits == right.bits;
}

/** @brief Rounds the divider up to an even value so @p hz stays an upper bound. */
[[nodiscard]] std::uint32_t divider(const std::uint8_t bus, const std::uint32_t hz) {
    const auto input = input_hz(bus);
    return ((input + hz * 2U - 1U) / (hz * 2U)) * 2U;
}

[[nodiscard]] SPI_TypeDef* prepare(const Lease& lease, const Format& format) {
    if (!lease.valid() || format.hz == 0U || format.hz > input_hz(lease.bus()) / 2U ||
        format.mode > 3U || (format.bits != 8U && format.bits != 9U))
        return nullptr;
    auto& bus = buses[lease.bus()];
    auto* const spi = device(lease.bus());
    if (!same(bus.format, format)) {
        SSI_Cmd(spi, DISABLE);
        SSI_SetSclkPolarity(
            spi, (format.mode & 2U) ? SCPOL_INACTIVE_IS_HIGH : SCPOL_INACTIVE_IS_LOW);
        SSI_SetSclkPhase(spi, (format.mode & 1U) ? SCPH_TOGGLES_AT_START : SCPH_TOGGLES_IN_MIDDLE);
        SSI_SetDataFrameSize(spi, format.bits - 1U);
        SSI_SetBaudDiv(spi, divider(lease.bus(), format.hz));
        SSI_Cmd(spi, ENABLE);
        bus.format = format;
    }
    return spi;
}

/** @brief Keeps TX and RX in step so the RX FIFO never overflows, then drains the wire. */
[[nodiscard]] Result clock_words(SPI_TypeDef* const spi, const std::uint8_t* const tx,
    std::uint8_t* const rx, const std::size_t size, const std::uint32_t high) {
    const auto start = monotonic_milliseconds();
    std::size_t sent = 0;
    std::size_t received = 0;
    while (received < size) {
        while (sent < size && sent - received < kInFlight && SSI_Writeable(spi)) {
            SSI_WriteData(spi, (tx != nullptr ? tx[sent] : 0xffU) | high);
            ++sent;
        }
        while (received < sent && SSI_Readable(spi)) {
            const auto word = SSI_ReadData(spi);
            if (rx != nullptr)
                rx[received] = static_cast<std::uint8_t>(word);
            ++received;
        }
        if (monotonic_milliseconds() - start >= kTimeoutMs)
            return Result::timeout;
    }
    while (SSI_Busy(spi)) {
        if (monotonic_milliseconds() - start >= kTimeoutMs)
            return Result::timeout;
    }
    return Result::success;
}

void release_pin(const int pin) {
    if (pin >= 0)
        (void)gpio_configure_input(PinId{static_cast<std::uint16_t>(pin)}, GpioPull::none);
}

} // namespace

Result preflight(const Wiring& wiring, const Format& format) {
    if (!route(wiring) || format.hz == 0U || format.hz > input_hz(wiring.bus) / 2U ||
        format.mode > 3U || (format.bits != 8U && format.bits != 9U))
        return Result::unsupported;
    return Result::success;
}

Result acquire(const Wiring& wiring, Lease& lease) {
    if (lease.valid())
        return Result::failed;
    if (!route(wiring))
        return Result::unsupported;
    auto& bus = buses[wiring.bus];
    if (bus.users != 0U) {
        if (!same(bus.wiring, wiring))
            return Result::conflict;
        if (bus.users == UINT8_MAX)
            return Result::busy;
    } else {
        auto* const spi = device(wiring.bus);
        if (spi->SSIENR)
            return Result::busy;
        RCC_PeriphClockCmd(wiring.bus != 0U ? APBPeriph_SPI1 : APBPeriph_SPI0,
            wiring.bus != 0U ? APBPeriph_SPI1_CLOCK : APBPeriph_SPI0_CLOCK, ENABLE);
        SSI_InitTypeDef config{};
        SSI_StructInit(&config);
        config.SPI_Role = SSI_MASTER;
        config.SPI_TransferMode = TMOD_TR;
        config.SPI_DataFrameSize = DFS_8_BITS;
        config.SPI_SclkPhase = SCPH_TOGGLES_IN_MIDDLE;
        config.SPI_SclkPolarity = SCPOL_INACTIVE_IS_LOW;
        config.SPI_ClockDivider = divider(wiring.bus, 400000U);
        config.SPI_SlaveSelectEnable = 1; // CS itself is a GPIO of each device.
        if (wiring.bus == 0U)
            SSI_SetRole(spi, SSI_MASTER); // SSI0 also has a slave role.
        SSI_Init(spi, &config);
        // The SDK names SSI0's function SPIS and SSI1's SPIM; both are pinmux function 3.
        const auto function = wiring.bus != 0U ? PINMUX_FUNCTION_SPIM : PINMUX_FUNCTION_SPIS;
        Pinmux_Config(wiring.clock, function);
        Pinmux_Config(wiring.mosi, function);
        if (wiring.miso >= 0)
            Pinmux_Config(wiring.miso, function);
        bus.wiring = wiring;
        bus.format = Format{400000U, 0U, 8U};
    }
    ++bus.users;
    lease.bus_ = wiring.bus;
    return Result::success;
}

Result release(Lease& lease) {
    if (!lease.valid() || buses[lease.bus()].users == 0U)
        return Result::failed;
    auto& bus = buses[lease.bus()];
    if (--bus.users == 0U) {
        SSI_Cmd(device(lease.bus()), DISABLE);
        release_pin(bus.wiring.clock);
        release_pin(bus.wiring.mosi);
        release_pin(bus.wiring.miso);
        bus = {};
    }
    lease = {};
    return Result::success;
}

Result exchange(const Lease& lease, const Format& format, const std::uint8_t* const tx,
    std::uint8_t* const rx, const std::size_t size) {
    if (format.bits != 8U)
        return Result::unsupported;
    auto* const spi = prepare(lease, format);
    if (spi == nullptr)
        return Result::failed;
    return clock_words(spi, tx, rx, size, 0U);
}

Result write_9bit(const Lease& lease, const Format& format, const bool high,
    const std::uint8_t* const bytes, const std::size_t size) {
    if (format.bits != 9U || (bytes == nullptr && size != 0U))
        return Result::unsupported;
    auto* const spi = prepare(lease, format);
    if (spi == nullptr)
        return Result::failed;
    return clock_words(spi, bytes, nullptr, size, high ? 0x100U : 0U);
}

} // namespace newmota::platform::spi_controller
