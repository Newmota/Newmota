/*
  spi_controller.cpp - Bouffalo SPI0 controller owner

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file spi_controller.cpp
 * @brief LHAL SPI0 master shared by devices with identical pins; 8-bit frames only.
 *
 * Pads carry SPI0 signals in a repeating group of four. The order differs per chip:
 * BL602 MOSI/SCLK/CS/MISO (unswapped), BL702 MOSI/CS/SCLK/MISO, BL616 and BL618DG
 * CS/SCLK/MISO/MOSI. The BL618DG order follows the SDK's bl618dgdk example and still needs
 * confirmation on hardware.
 */
#include "newmota/platform/spi_controller.h"

#include "newmota/platform/gpio.h"
#include "newmota/platform/platform.h"
extern "C" {
#include "bflb_clock.h"
#include "bflb_gpio.h"
#include "bflb_spi.h"
#include "hardware/spi_reg.h"
}

#include <array>

namespace newmota::platform::spi_controller {
namespace {

constexpr std::uint32_t kMaximumHz = 40000000U;
constexpr std::uint32_t kTimeoutMs = 100U;

#if defined(BL602)
constexpr int kPins = 23;
constexpr unsigned kMosi = 0, kClock = 1, kMiso = 3;
#elif defined(BL702)
constexpr int kPins = 38;
constexpr unsigned kMosi = 0, kClock = 2, kMiso = 3;
#elif defined(BL616)
constexpr int kPins = 35;
constexpr unsigned kMosi = 3, kClock = 1, kMiso = 2;
#elif defined(BL618DG)
constexpr int kPins = 53;
constexpr unsigned kMosi = 3, kClock = 1, kMiso = 2;
#else
#error "unknown Bouffalo chip for SPI0 pad routes"
#endif

struct Bus final {
    bflb_device_s* device{};
    Wiring wiring{};
    Format format{};
    std::uint8_t users{};
};

Bus spi0{};

[[nodiscard]] bool route(const int pin, const unsigned signal) {
    return pin >= 0 && pin < kPins && (static_cast<unsigned>(pin) & 3U) == signal;
}

[[nodiscard]] bool same(const Wiring& left, const Wiring& right) {
    return left.clock == right.clock && left.mosi == right.mosi && left.miso == right.miso;
}

[[nodiscard]] bool same(const Format& left, const Format& right) {
    return left.hz == right.hz && left.mode == right.mode && left.bits == right.bits;
}

[[nodiscard]] bool wait_idle(bflb_device_s* const device) {
    const auto start = monotonic_milliseconds();
    while (bflb_spi_isbusy(device))
        if (monotonic_milliseconds() - start >= kTimeoutMs)
            return false;
    return true;
}

/**
 * @brief Returns the rate to hand LHAL so that its divider never exceeds @p hz.
 *
 * LHAL rounds half the SPI clock divided by the request to the nearest divider. The exact
 * rate of the rounded-up divider makes that rounding land on it; 0 means @p hz is below the
 * slowest divider (256).
 */
[[nodiscard]] std::uint32_t ceiling_rate(bflb_device_s* const device, const std::uint32_t hz) {
    const auto half = bflb_clk_get_peripheral_clock(BFLB_DEVICE_TYPE_SPI, device->idx) / 2U;
    const auto divider = (half + hz - 1U) / hz;
    return divider == 0U || divider > 256U ? 0U : half / divider;
}

void release_pin(const int pin) {
    if (pin >= 0)
        (void)gpio_configure_input(PinId{static_cast<std::uint16_t>(pin)}, GpioPull::none);
}

} // namespace

Result preflight(const Wiring& wiring, const Format& format) {
    if (wiring.bus != 0U || !route(wiring.clock, kClock) || !route(wiring.mosi, kMosi) ||
        (wiring.miso >= 0 && !route(wiring.miso, kMiso)) || format.hz == 0U ||
        format.hz > kMaximumHz || format.mode > 3U || format.bits != 8U)
        return Result::unsupported;
    return Result::success;
}

Result acquire(const Wiring& wiring, Lease& lease) {
    if (lease.valid())
        return Result::failed;
    if (preflight(wiring, Format{400000U, 0U, 8U}) != Result::success)
        return Result::unsupported;
    if (spi0.users != 0U) {
        if (!same(spi0.wiring, wiring))
            return Result::conflict;
        if (spi0.users == UINT8_MAX)
            return Result::busy;
    } else {
        auto* const device = bflb_device_get_by_name("spi0");
        if (device == nullptr)
            return Result::unsupported;
        if (getreg32(device->reg_base + SPI_CONFIG_OFFSET) & SPI_CR_SPI_M_EN)
            return Result::busy;
        const auto identify = ceiling_rate(device, 400000U);
        if (identify == 0U)
            return Result::unsupported;
        bflb_spi_config_s config{};
        config.freq = identify;
        config.role = SPI_ROLE_MASTER;
        config.mode = SPI_MODE0;
        config.data_width = SPI_DATA_WIDTH_8BIT;
        config.bit_order = SPI_BIT_MSB;
        config.byte_order = SPI_BYTE_LSB;
        config.tx_fifo_threshold = 1;
        bflb_spi_init(device, &config);
        auto* gpio = bflb_device_get_by_name("gpio");
        const auto output = GPIO_FUNC_SPI0 | GPIO_ALTERNATE | GPIO_FLOAT | GPIO_DRV_1;
        bflb_gpio_init(gpio, wiring.clock, output);
        bflb_gpio_init(gpio, wiring.mosi, output);
        if (wiring.miso >= 0) // An SD card releases MISO between responses.
            bflb_gpio_init(gpio, wiring.miso,
                GPIO_FUNC_SPI0 | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
        spi0.device = device;
        spi0.wiring = wiring;
        spi0.format = Format{400000U, 0U, 8U};
    }
    ++spi0.users;
    lease.bus_ = 0U;
    return Result::success;
}

Result release(Lease& lease) {
    if (!lease.valid() || spi0.users == 0U)
        return Result::failed;
    if (--spi0.users == 0U) {
        bflb_spi_deinit(spi0.device);
        release_pin(spi0.wiring.clock);
        release_pin(spi0.wiring.mosi);
        release_pin(spi0.wiring.miso);
        spi0 = {};
    }
    lease = {};
    return Result::success;
}

Result exchange(const Lease& lease, const Format& format, const std::uint8_t* const tx,
    std::uint8_t* const rx, const std::size_t size) {
    if (!lease.valid() || spi0.device == nullptr ||
        preflight(spi0.wiring, format) != Result::success)
        return format.bits != 8U ? Result::unsupported : Result::failed;
    auto* const device = spi0.device;
    if (!same(spi0.format, format)) {
        if (!wait_idle(device))
            return Result::timeout;
        const auto rate = ceiling_rate(device, format.hz);
        if (rate == 0U)
            return Result::unsupported;
        bflb_spi_feature_control(device, SPI_CMD_SET_FREQ, rate);
        bflb_spi_feature_control(device, SPI_CMD_SET_MODE, format.mode);
        spi0.format = format;
    }
    if (tx != nullptr || size == 0U) {
        if (size != 0U && bflb_spi_poll_exchange(device, tx, rx, size) < 0)
            return Result::failed;
    } else {
        // The ROM exchange's fill byte is unspecified; SD reads need MOSI high.
        std::array<std::uint8_t, 64> idle{};
        idle.fill(0xff);
        for (std::size_t done = 0; done < size; done += idle.size()) {
            const auto chunk = size - done < idle.size() ? size - done : idle.size();
            if (bflb_spi_poll_exchange(
                    device, idle.data(), rx != nullptr ? rx + done : nullptr, chunk) < 0)
                return Result::failed;
        }
    }
    return wait_idle(device) ? Result::success : Result::timeout;
}

Result write_9bit(const Lease&, const Format&, bool, const std::uint8_t*, std::size_t) {
    return Result::unsupported; // SPI0 frames are 8, 16, 24 or 32 bits.
}

} // namespace newmota::platform::spi_controller
