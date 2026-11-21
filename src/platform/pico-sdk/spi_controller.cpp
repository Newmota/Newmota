/*
  spi_controller.cpp - Pico SDK SPI controller owner

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file spi_controller.cpp
 * @brief SPI0/SPI1 shared by devices with identical pins; CS stays a GPIO.
 */
#include "newmota/platform/spi_controller.h"

#include "newmota/platform/gpio.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"

#include <array>

namespace newmota::platform::spi_controller {
namespace {

struct Bus final {
    Wiring wiring{};
    Format format{};
    std::uint8_t users{};
};

std::array<Bus, 2> buses{};

[[nodiscard]] spi_inst_t* instance(const std::uint8_t bus) {
    return bus == 0U ? spi0 : spi1;
}

/** @brief Bank-0 SPI functions repeat RX, CSn, SCK, TX every four pins, the bus every eight. */
[[nodiscard]] bool route(const int pin, const unsigned signal, const unsigned bus) {
    return pin >= 0 && pin < NUM_BANK0_GPIOS && (static_cast<unsigned>(pin) & 3U) == signal &&
           ((static_cast<unsigned>(pin) >> 3U) & 1U) == bus;
}

[[nodiscard]] bool same(const Wiring& left, const Wiring& right) {
    return left.clock == right.clock && left.mosi == right.mosi && left.miso == right.miso;
}

[[nodiscard]] bool same(const Format& left, const Format& right) {
    return left.hz == right.hz && left.mode == right.mode && left.bits == right.bits;
}

/** @brief Programs rate and framing when the previous transfer used another format. */
[[nodiscard]] spi_inst_t* prepare(const Lease& lease, const Format& format) {
    if (!lease.valid() || format.hz == 0U || format.mode > 3U ||
        (format.bits != 8U && format.bits != 9U))
        return nullptr;
    auto& bus = buses[lease.bus()];
    auto* const device = instance(lease.bus());
    if (!same(bus.format, format)) {
        spi_set_baudrate(device, format.hz); // Rounds down to the nearest divider.
        spi_set_format(device, format.bits, (format.mode & 2U) ? SPI_CPOL_1 : SPI_CPOL_0,
            (format.mode & 1U) ? SPI_CPHA_1 : SPI_CPHA_0, SPI_MSB_FIRST);
        bus.format = format;
    }
    return device;
}

void release_pin(const int pin) {
    if (pin >= 0)
        (void)gpio_configure_input(PinId{static_cast<std::uint16_t>(pin)}, GpioPull::none);
}

} // namespace

Result preflight(const Wiring& wiring, const Format& format) {
    if (wiring.bus > 1U || !route(wiring.clock, 2U, wiring.bus) ||
        !route(wiring.mosi, 3U, wiring.bus) ||
        (wiring.miso >= 0 && !route(wiring.miso, 0U, wiring.bus)) || format.hz == 0U ||
        format.hz > clock_get_hz(clk_peri) / 2U || format.mode > 3U ||
        (format.bits != 8U && format.bits != 9U))
        return Result::unsupported;
    return Result::success;
}

Result acquire(const Wiring& wiring, Lease& lease) {
    if (lease.valid() || preflight(wiring, Format{400000U, 0U, 8U}) != Result::success)
        return lease.valid() ? Result::failed : Result::unsupported;
    auto& bus = buses[wiring.bus];
    if (bus.users != 0U) {
        if (!same(bus.wiring, wiring))
            return Result::conflict;
        if (bus.users == UINT8_MAX)
            return Result::busy;
    } else {
        auto* const device = instance(wiring.bus);
        if (spi_get_hw(device)->cr1 & SPI_SSPCR1_SSE_BITS)
            return Result::busy;
        spi_init(device, 400000U);
        bus.format = Format{};
        bus.wiring = wiring;
        gpio_set_function(wiring.clock, GPIO_FUNC_SPI);
        gpio_set_function(wiring.mosi, GPIO_FUNC_SPI);
        if (wiring.miso >= 0)
            gpio_set_function(wiring.miso, GPIO_FUNC_SPI);
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
        spi_deinit(instance(lease.bus()));
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
    auto* const device = prepare(lease, format);
    if (device == nullptr)
        return Result::failed;
    if (size == 0U)
        return Result::success;
    const auto length = static_cast<int>(size);
    int done = 0;
    if (tx != nullptr && rx != nullptr)
        done = spi_write_read_blocking(device, tx, rx, size);
    else if (tx != nullptr)
        done = spi_write_blocking(device, tx, size);
    else if (rx != nullptr)
        done = spi_read_blocking(device, 0xff, rx, size);
    else {
        static constexpr std::uint8_t kIdle[16]{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        for (std::size_t sent = 0; sent < size; sent += sizeof(kIdle)) {
            const auto chunk = size - sent < sizeof(kIdle) ? size - sent : sizeof(kIdle);
            done += spi_write_blocking(device, kIdle, chunk);
        }
    }
    return done == length ? Result::success : Result::failed;
}

Result write_9bit(const Lease& lease, const Format& format, const bool high,
    const std::uint8_t* const bytes, const std::size_t size) {
    if (format.bits != 9U || (bytes == nullptr && size != 0U))
        return Result::unsupported;
    auto* const device = prepare(lease, format);
    if (device == nullptr)
        return Result::failed;
    for (std::size_t index = 0; index < size; ++index) {
        const std::uint16_t word = bytes[index] | (high ? 0x100U : 0U);
        if (spi_write16_blocking(device, &word, 1) != 1)
            return Result::failed;
    }
    return Result::success;
}

} // namespace newmota::platform::spi_controller
