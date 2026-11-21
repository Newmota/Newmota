/*
  native_spi.cpp - display SPI transport over the shared SPI controller owner

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file native_spi.cpp
 * @brief Native display SPI for Bouffalo, AmebaD and Pico: manual CS, mode 3, 8 or 9 bits.
 *
 * The controller comes from spi_controller, so an SD card with the same clock, MOSI and MISO
 * pins can share it. A 3-wire panel (no DC pin) sends 9-bit words where the controller has
 * them. The lease is retained until restart, as the display owner expects.
 */
#include "newmota/platform/display/native.h"
#include "newmota/platform/spi_controller.h"

namespace newmota::platform::display::detail {
namespace {
spi_controller::Lease lease{};
spi_controller::Format format{};

[[nodiscard]] spi_controller::Wiring wiring(const Config& c) {
    return {static_cast<std::uint8_t>(c.spi_bus - 1), c.clock, c.mosi, c.miso};
}

[[nodiscard]] spi_controller::Format panel_format(const Config& c) {
    return {static_cast<std::uint32_t>(c.speed) * 1000000U, 3U,
        static_cast<std::uint8_t>(c.dc < 0 ? 9U : 8U)};
}

[[nodiscard]] Result result(const spi_controller::Result value) {
    switch (value) {
    case spi_controller::Result::success:
        return Result::success;
    case spi_controller::Result::unsupported:
        return Result::unsupported;
    case spi_controller::Result::busy:
    case spi_controller::Result::conflict:
        return Result::busy;
    case spi_controller::Result::timeout:
        return Result::timeout;
    case spi_controller::Result::failed:
        break;
    }
    return Result::failed;
}
} // namespace

Result native_spi_preflight(const Config& c) {
    if (c.spi_bus < 1 || c.speed <= 0)
        return Result::unsupported;
    return result(spi_controller::preflight(wiring(c), panel_format(c)));
}

Result native_spi_open(const Config& c) {
    const auto acquired = result(spi_controller::acquire(wiring(c), lease));
    if (acquired == Result::success)
        format = panel_format(c);
    return acquired;
}

Result native_spi_write(bool data, const std::uint8_t* bytes, std::size_t length) {
    if (!lease.valid())
        return Result::failed;
    return result(format.bits == 9U
                      ? spi_controller::write_9bit(lease, format, data, bytes, length)
                      : spi_controller::exchange(lease, format, bytes, nullptr, length));
}
} // namespace newmota::platform::display::detail
