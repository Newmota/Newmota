/*
  ethernet_service.cpp - Tasmota's Ethernet settings and commands for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#include "services/network/ethernet_service.h"

#include "core/storage/driver_settings_file.h"
#include "core/system/restart.h"
#include "gpio_ids.h"
#include "newmota/log.h"
#include "newmota/platform/ethernet.h"
#include "newmota/platform/platform.h"
#include "support/number.h"
#include "support/text.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace newmota::ethernet {
namespace {
namespace eth = platform::ethernet;

/** @brief On/off, EthType, EthAddress, EthClockMode, then five IPv4 values. */
constexpr std::size_t kIpv4Count = 5;
constexpr std::size_t kPayloadSize = 4 + kIpv4Count * 4;
static_assert(kPayloadSize <= kDriverSettingsMaximumPayload);

struct Stored final {
    bool enabled{true}; // Tasmota's flag4.network_ethernet default
    std::uint8_t type{};
    std::int8_t address{};
    std::uint8_t clock_mode{};
    std::array<std::uint32_t, kIpv4Count> ipv4{}; // network order; address, gateway,
                                                  // netmask, DNS 1, DNS 2
};

bool valid_payload(const std::uint8_t* payload) {
    const auto address = static_cast<std::int8_t>(payload[2]);
    return payload[0] <= 1U && payload[1] < eth::kPhyCount && address >= -1 && address <= 31 &&
           payload[3] <= 3U;
}

constexpr DriverSettingsSpec kSpec{"/.newmota/ethernet.bin", "/.newmota/ethernet.tmp",
    "/.newmota/ethernet.prev", {'N', 'M', 'E', 'T'}, 1, kPayloadSize, valid_payload};

std::array<std::uint8_t, kPayloadSize> encode(const Stored& value) {
    std::array<std::uint8_t, kPayloadSize> payload{};
    payload[0] = value.enabled ? 1U : 0U;
    payload[1] = value.type;
    payload[2] = static_cast<std::uint8_t>(value.address);
    payload[3] = value.clock_mode;
    std::memcpy(payload.data() + 4, value.ipv4.data(), kIpv4Count * 4);
    return payload;
}

Stored decode(const std::uint8_t* payload) {
    Stored value{};
    value.enabled = payload[0] != 0U;
    value.type = payload[1];
    value.address = static_cast<std::int8_t>(payload[2]);
    value.clock_mode = payload[3];
    std::memcpy(value.ipv4.data(), payload + 4, kIpv4Count * 4);
    return value;
}

Stored stored{};
RestartController* restarter{};
bool running{};

eth::Ipv4 ipv4() {
    return {stored.ipv4[0], stored.ipv4[1], stored.ipv4[2], stored.ipv4[3], stored.ipv4[4]};
}

/** @brief The pad of one active function instance, or none. */
PinId assigned(const Hardware& hardware, const std::uint16_t base, const std::uint8_t instance) {
    const auto function = generated::gpio::encode(base, instance);
    for (std::size_t index = 0; index < hardware.binding_count(); ++index) {
        const auto* binding = hardware.binding(index);
        if (binding->function == function && binding->active())
            return binding->pin;
    }
    return PinId{};
}

/** @brief Dotted IPv4 into network order, as Tasmota's ParseIPv4. */
bool parse_ipv4(const std::string_view text, std::uint32_t& value) {
    std::uint8_t bytes[4]{};
    std::size_t start = 0;
    for (std::size_t part = 0; part < 4U; ++part) {
        const auto end = part == 3U ? text.size() : text.find('.', start);
        std::uint32_t number{};
        if (end == std::string_view::npos ||
            !support::parse_unsigned_decimal(text.substr(start, end - start), 255U, number))
            return false;
        bytes[part] = static_cast<std::uint8_t>(number);
        start = end + 1U;
    }
    std::memcpy(&value, bytes, sizeof(value));
    return true;
}

/** @brief Saves and, like Tasmota's restart_flag 2, restarts for hardware settings. */
bool save(const bool restart) {
    if (!save_driver_settings(kSpec, encode(stored).data()))
        return false;
    if (restart && restarter != nullptr)
        (void)restarter->schedule(RestartTarget::current, platform::monotonic_milliseconds());
    return true;
}

CommandStatus number(const CommandRequest& request, CommandOutput& output, const int value) {
    return output.format("{\"%.*s\":%d}", int(request.name.size()), request.name.data(), value)
               ? CommandStatus::handled
               : CommandStatus::failed;
}

/** @brief EthIPAddress, EthGateway, EthSubnetmask and EthDNSServer1/2. */
CommandStatus ip_command(
    const CommandRequest& request, CommandOutput& output, std::size_t slot, const bool dns) {
    if (dns) {
        const auto index = request.has_index && request.index >= 1U && request.index <= 2U
                               ? request.index
                               : 1U; // Tasmota treats other indexes as 1
        slot += index - 1U;
    } else if (request.has_index) {
        return CommandStatus::invalid;
    }
    if (!request.is_query()) {
        std::uint32_t value{};
        if (!parse_ipv4(request.payload, value))
            return CommandStatus::invalid;
        stored.ipv4[slot] = value;
        if (!save(false))
            return CommandStatus::failed;
        if (running)
            (void)eth::set_ipv4(ipv4());
    }
    char address[16]{};
    platform::format_ipv4(address, &stored.ipv4[slot]);
    char current[24]{};
    if (slot == 0U && stored.ipv4[0] == 0U) { // DHCP shows the leased address as well
        const auto info = eth::info();
        std::snprintf(
            current, sizeof(current), " (%s)", info.address[0] ? info.address : "0.0.0.0");
    }
    const unsigned suffix = dns ? unsigned(slot - 2U) : 0U;
    return output.format("{\"%.*s", int(request.name.size()), request.name.data()) &&
                   (!dns || output.format("%u", suffix)) &&
                   output.format("\":\"%s%s\"}", address, current)
               ? CommandStatus::handled
               : CommandStatus::failed;
}
} // namespace

void initialize(
    const Hardware& hardware, const std::string_view hostname, RestartController& restart) {
    restarter = &restart;
    auto payload = encode(Stored{});
    (void)load_driver_settings(kSpec, payload.data());
    stored = decode(payload.data());
    if (!stored.enabled)
        return;
    eth::Config config{};
    config.phy = static_cast<eth::Phy>(stored.type);
    config.address = stored.address;
    config.clock_mode = stored.clock_mode;
    // The MDC instance names the SPI bus: ETH MDC1 on SPI1, ETH MDC2 on SPI2.
    for (std::uint8_t instance = 1; instance <= 2U && !config.mdc.valid(); ++instance) {
        config.mdc = assigned(hardware, generated::gpio::kGpioEthPhyMdcBase, instance);
        config.spi_bus = static_cast<std::uint8_t>(instance - 1U);
    }
    config.mdio = assigned(hardware, generated::gpio::kGpioEthPhyMdioBase, 1U);
    config.power = assigned(hardware, generated::gpio::kGpioEthPhyPowerBase, 1U);
    if (eth::uses_spi(config.phy)) {
        const auto bus = static_cast<std::uint8_t>(config.spi_bus + 1U);
        config.spi_clk = assigned(hardware, generated::gpio::kGpioSpiClkBase, bus);
        config.spi_miso = assigned(hardware, generated::gpio::kGpioSpiMisoBase, bus);
        config.spi_mosi = assigned(hardware, generated::gpio::kGpioSpiMosiBase, bus);
        if (!config.mdc.valid() || !config.spi_clk.valid() || !config.spi_miso.valid() ||
            !config.spi_mosi.valid())
            return; // Tasmota: no ETH MDC as SPI CS, or no SPI bus GPIO defined
    } else if (!config.mdc.valid() && !config.mdio.valid()) {
        return; // Tasmota: no ETH MDC and ETH MDIO GPIO defined
    }
    running = eth::start(config, ipv4(), hostname) == eth::Result::success;
}

CommandStatus command(const CommandRequest& request, CommandOutput& output) {
    const auto name = request.name;
    if (name.size() < 3U || !support::ascii_iequals(name.substr(0, 3), "Eth"))
        return CommandStatus::unknown;
    const auto rest = name.substr(3);
    if (support::ascii_iequals(rest, "IPAddress"))
        return ip_command(request, output, 0U, false);
    if (support::ascii_iequals(rest, "Gateway"))
        return ip_command(request, output, 1U, false);
    if (support::ascii_iequals(rest, "Subnetmask"))
        return ip_command(request, output, 2U, false);
    if (support::ascii_iequals(rest, "DNSServer"))
        return ip_command(request, output, 3U, true);
    const bool enable = support::ascii_iequals(rest, "ernet");
    const bool address = support::ascii_iequals(rest, "Address");
    const bool type = support::ascii_iequals(rest, "Type");
    const bool clock = support::ascii_iequals(rest, "ClockMode");
    if (!enable && !address && !type && !clock)
        return CommandStatus::unknown;
    if (request.has_index)
        return CommandStatus::invalid;
    if (!request.is_query()) {
        std::int32_t value{};
        const std::int32_t minimum = address ? -1 : 0;
        const std::int32_t maximum = enable ? 1 : address ? 31 : type ? eth::kPhyCount - 1 : 3;
        if (enable && (support::ascii_iequals(request.payload, "on") ||
                          support::ascii_iequals(request.payload, "off"))) {
            value = support::ascii_iequals(request.payload, "on") ? 1 : 0;
        } else if (!support::parse_signed_decimal(request.payload, minimum, maximum, value)) {
            return CommandStatus::invalid;
        }
        if (enable)
            stored.enabled = value != 0;
        else if (address)
            stored.address = static_cast<std::int8_t>(value);
        else if (type)
            stored.type = static_cast<std::uint8_t>(value);
        else
            stored.clock_mode = static_cast<std::uint8_t>(value);
        if (!save(true))
            return CommandStatus::failed;
    }
    if (enable)
        return output.format("{\"Ethernet\":\"%s\"}", stored.enabled ? "ON" : "OFF")
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    return number(request, output,
        address ? stored.address
        : type  ? stored.type
                : static_cast<int>(stored.clock_mode));
}

bool connected() {
    return running && eth::connected();
}

bool info(platform::NetworkInfo& info) {
    if (!running)
        return false;
    info = eth::info();
    return true;
}

} // namespace newmota::ethernet
