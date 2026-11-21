/*
  ethernet.h - Wired Ethernet for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ethernet.h
 * @brief Ported from Tasmota's xdrv_82_esp32_ethernet.ino.
 *
 * One interface beside Wi-Fi: an RMII PHY on the chip's own MAC (ESP32, P4, S31) or an SPI
 * controller on any ESP chip. Tasmota's EthType numbers name the chips. The interface needs
 * the network stack that the Wi-Fi adapter initialises, so it starts after Wi-Fi. Every call
 * belongs to the application task.
 */
#pragma once
#include "newmota/core/hardware/pin.h"
#include "newmota/platform/network.h"

#include <cstdint>
#include <string_view>

namespace newmota::platform::ethernet {

/** @brief Tasmota's EthType values. */
enum class Phy : std::uint8_t {
    lan8720 = 0,
    ip101 = 1, // and TLK110
    rtl8201 = 2,
    dp83848 = 3,
    dm9051 = 4,
    ksz8081 = 5,
    ksz8041 = 6,
    jl1101 = 7,
    w5500 = 8,
    ksz8851 = 9,
};
inline constexpr std::uint8_t kPhyCount = 10;

/** @brief Whether @p phy talks SPI; then MDC is chip select, MDIO the interrupt and power
 * the reset, as in Tasmota. */
[[nodiscard]] constexpr bool uses_spi(const Phy phy) {
    return phy == Phy::dm9051 || phy == Phy::w5500 || phy == Phy::ksz8851;
}

/** @brief Whether this build can drive @p phy on this chip. */
[[nodiscard]] bool supported(Phy phy);

struct Config final {
    Phy phy{Phy::lan8720};
    /** @brief PHY address 0..31, or -1 to probe. */
    std::int8_t address{};
    /** @brief Tasmota's EthClockMode on the classic ESP32: 0 GPIO0 in, 1 GPIO0 out, 2 GPIO16
     * out, 3 GPIO17 inverted out. Other chips use their board default. */
    std::uint8_t clock_mode{};
    PinId mdc{}, mdio{}, power{};
    /** @brief SPI controllers: bus 0 or 1 and its wires. */
    std::uint8_t spi_bus{};
    PinId spi_clk{}, spi_miso{}, spi_mosi{};
};

/** @brief Static addressing in network byte order; a zero address keeps DHCP. */
struct Ipv4 final {
    std::uint32_t address{}, gateway{}, netmask{}, dns1{}, dns2{};
};

enum class Result : std::uint8_t { success, unavailable, invalid_argument, failed };

/** @brief Creates the driver and its interface and starts the link. */
[[nodiscard]] Result start(const Config& config, const Ipv4& ipv4, std::string_view hostname);

/** @brief Switches between DHCP and the given static addressing on the running interface. */
[[nodiscard]] Result set_ipv4(const Ipv4& ipv4);

/** @brief Whether the interface has an IPv4 address. */
[[nodiscard]] bool connected();

/** @brief Hostname, addressing and MAC of the interface, as for StatusNET. */
[[nodiscard]] NetworkInfo info();

} // namespace newmota::platform::ethernet
