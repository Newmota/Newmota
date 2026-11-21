/*
  ethernet.cpp - ESP-IDF wired Ethernet for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ethernet.cpp
 * @brief Ported from Tasmota's xdrv_82_esp32_ethernet.ino and Arduino's ETH.cpp.
 *
 * ESP-IDF 5.5 carries every driver in esp_eth. From 6.0 the PHY and SPI drivers are
 * registry components; IDF 6.1 builds compile the vendored copies in
 * third_party/esp_eth_drivers, whose only API change is W5500's nested base config.
 */
#include "newmota/platform/ethernet.h"

#include "newmota/log.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "spi_host.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "sdkconfig.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#define NEWMOTA_ETH_REGISTRY_DRIVERS 1
#include "esp_eth_mac_dm9051.h"
#include "esp_eth_mac_ksz8851snl.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_dm9051.h"
#include "esp_eth_phy_dp83848.h"
#include "esp_eth_phy_ip101.h"
#include "esp_eth_phy_ksz80xx.h"
#include "esp_eth_phy_ksz8851snl.h"
#include "esp_eth_phy_lan87xx.h"
#include "esp_eth_phy_rtl8201.h"
#include "esp_eth_phy_w5500.h"
#else
#define NEWMOTA_ETH_REGISTRY_DRIVERS 0
#endif

#include <atomic>
#include <cstdio>
#include <cstring>

namespace newmota::platform::ethernet {
namespace {
constexpr int kSpiClockHz = 20 * 1000 * 1000; // Arduino's ETH_PHY_SPI_FREQ_MHZ
constexpr std::uint32_t kPollMs = 10;         // SPI controllers without an interrupt pin

esp_eth_handle_t handle{};
esp_netif_t* netif{};
spi_host::Lease spi_lease{}; // Held while the SPI controller's driver runs.
// The event task sets these, the application task reads them; online needs both.
std::atomic<bool> link_up{};
std::atomic<bool> has_address{};
// Configured DNS overrides; a DHCP lease replaces the servers, so each lease re-applies them.
std::atomic<std::uint32_t> dns_servers[2]{};

#if CONFIG_ETH_USE_ESP32_EMAC
constexpr bool kEmac = true;
#else
constexpr bool kEmac = false;
#endif
#if NEWMOTA_ETH_REGISTRY_DRIVERS || CONFIG_ETH_SPI_ETHERNET_DM9051
constexpr bool kDm9051 = true;
#else
constexpr bool kDm9051 = false;
#endif
#if NEWMOTA_ETH_REGISTRY_DRIVERS || CONFIG_ETH_SPI_ETHERNET_W5500
constexpr bool kW5500 = true;
#else
constexpr bool kW5500 = false;
#endif
#if NEWMOTA_ETH_REGISTRY_DRIVERS || CONFIG_ETH_SPI_ETHERNET_KSZ8851SNL
constexpr bool kKsz8851 = true;
#else
constexpr bool kKsz8851 = false;
#endif

int gpio(const PinId pin) {
    return pin.valid() ? pin.value() : -1;
}

void apply_dns() {
    for (int index = 0; index < 2; ++index) {
        const auto server = dns_servers[index].load();
        if (server == 0U)
            continue;
        esp_netif_dns_info_t dns{};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = server;
        (void)esp_netif_set_dns_info(netif, static_cast<esp_netif_dns_type_t>(index), &dns);
    }
}

void event(void*, const esp_event_base_t base, const std::int32_t id, void* const data) {
    if (base == ETH_EVENT && id == ETHERNET_EVENT_CONNECTED) {
        eth_speed_t speed{};
        eth_duplex_t duplex{};
        (void)esp_eth_ioctl(handle, ETH_CMD_G_SPEED, &speed);
        (void)esp_eth_ioctl(handle, ETH_CMD_G_DUPLEX_MODE, &duplex);
        link_up.store(true);
        AddLog(LogLevel::info, "ETH: Connected at %uMbps%s", speed == ETH_SPEED_100M ? 100U : 10U,
            duplex == ETH_DUPLEX_FULL ? " Full Duplex" : "");
    } else if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED) {
        link_up.store(false); // a static address stays; DHCP reports LOST_IP on its own
        AddLog(LogLevel::info, "ETH: Disconnected");
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        const auto* got = static_cast<const ip_event_got_ip_t*>(data);
        char address[16]{};
        format_ipv4(address, &got->ip_info.ip.addr);
        AddLog(LogLevel::info, "ETH: IPv4 %s", address);
        apply_dns();
        has_address.store(true);
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_LOST_IP) {
        has_address.store(false);
    }
}

#if CONFIG_ETH_USE_ESP32_EMAC
esp_eth_mac_t* new_emac(const Config& config, const eth_mac_config_t& mac) {
    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac.smi_gpio.mdc_num = gpio(config.mdc);
    emac.smi_gpio.mdio_num = gpio(config.mdio);
#if CONFIG_IDF_TARGET_ESP32
    // Tasmota's EthClockMode; the other chips take the reference clock in, as Tasmota does.
    using ClockGpio = decltype(emac.clock_config.rmii.clock_gpio);
    static constexpr int kClockGpio[] = {0, 0, 16, 17};
    emac.clock_config.rmii.clock_mode = config.clock_mode == 0 ? EMAC_CLK_EXT_IN : EMAC_CLK_OUT;
    emac.clock_config.rmii.clock_gpio = static_cast<ClockGpio>(kClockGpio[config.clock_mode & 3U]);
#endif
    return esp_eth_mac_new_esp32(&emac, &mac);
}
#endif

esp_eth_phy_t* new_phy(const Config& config, eth_phy_config_t& phy) {
    switch (config.phy) {
#if CONFIG_ETH_USE_ESP32_EMAC
    case Phy::lan8720:
        return esp_eth_phy_new_lan87xx(&phy);
    case Phy::ip101:
        return esp_eth_phy_new_ip101(&phy);
    case Phy::rtl8201:
        return esp_eth_phy_new_rtl8201(&phy);
    case Phy::dp83848:
        return esp_eth_phy_new_dp83848(&phy);
    case Phy::ksz8081:
    case Phy::ksz8041:
        return esp_eth_phy_new_ksz80xx(&phy);
    case Phy::jl1101: // Arduino ships its own driver; the generic 802.3 PHY covers it
        return esp_eth_phy_new_generic(&phy);
#endif
#if NEWMOTA_ETH_REGISTRY_DRIVERS || CONFIG_ETH_SPI_ETHERNET_DM9051
    case Phy::dm9051:
        return esp_eth_phy_new_dm9051(&phy);
#endif
#if NEWMOTA_ETH_REGISTRY_DRIVERS || CONFIG_ETH_SPI_ETHERNET_W5500
    case Phy::w5500:
        return esp_eth_phy_new_w5500(&phy);
#endif
#if NEWMOTA_ETH_REGISTRY_DRIVERS || CONFIG_ETH_SPI_ETHERNET_KSZ8851SNL
    case Phy::ksz8851:
        return esp_eth_phy_new_ksz8851snl(&phy);
#endif
    default:
        return nullptr;
    }
}

/** @brief Starts the SPI bus and creates the controller's MAC; the PHY sits on the chip. */
esp_eth_mac_t* new_spi_mac(const Config& config, const eth_mac_config_t& mac) {
#if SOC_SPI_PERIPH_NUM > 2
    const auto host = config.spi_bus == 1U ? SPI3_HOST : SPI2_HOST;
#else
    if (config.spi_bus != 0U)
        return nullptr;
    const auto host = SPI2_HOST;
#endif
    spi_bus_config_t bus{};
    bus.sclk_io_num = gpio(config.spi_clk);
    bus.miso_io_num = gpio(config.spi_miso);
    bus.mosi_io_num = gpio(config.spi_mosi);
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    // A display or SD card may share these wires; each device keeps its own CS.
    if (!spi_lease.valid() &&
        spi_host::acquire(host, bus, spi_host::Use::shared, spi_lease) != spi_host::Result::success)
        return nullptr;
    const int interrupt = gpio(config.mdio);
    if (interrupt >= 0) {
        const auto isr = gpio_install_isr_service(0);
        if (isr != ESP_OK && isr != ESP_ERR_INVALID_STATE)
            return nullptr;
    }
    static spi_device_interface_config_t device{};
    device = {};
    device.mode = 0;
    device.clock_speed_hz = kSpiClockHz;
    device.queue_size = 20;
    device.spics_io_num = gpio(config.mdc);
    const std::uint32_t poll = interrupt < 0 ? kPollMs : 0U;
    switch (config.phy) {
#if NEWMOTA_ETH_REGISTRY_DRIVERS || CONFIG_ETH_SPI_ETHERNET_DM9051
    case Phy::dm9051: {
        eth_dm9051_config_t dm9051 = ETH_DM9051_DEFAULT_CONFIG(host, &device);
        dm9051.int_gpio_num = interrupt;
        dm9051.poll_period_ms = poll;
        return esp_eth_mac_new_dm9051(&dm9051, &mac);
    }
#endif
#if NEWMOTA_ETH_REGISTRY_DRIVERS || CONFIG_ETH_SPI_ETHERNET_W5500
    case Phy::w5500: {
        eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(host, &device);
#if NEWMOTA_ETH_REGISTRY_DRIVERS
        w5500.base.int_gpio_num = interrupt;
        w5500.base.poll_period_ms = poll;
#else
        w5500.int_gpio_num = interrupt;
        w5500.poll_period_ms = poll;
#endif
        return esp_eth_mac_new_w5500(&w5500, &mac);
    }
#endif
#if NEWMOTA_ETH_REGISTRY_DRIVERS || CONFIG_ETH_SPI_ETHERNET_KSZ8851SNL
    case Phy::ksz8851: {
        eth_ksz8851snl_config_t ksz8851 = ETH_KSZ8851SNL_DEFAULT_CONFIG(host, &device);
        ksz8851.int_gpio_num = interrupt;
        ksz8851.poll_period_ms = poll;
        return esp_eth_mac_new_ksz8851snl(&ksz8851, &mac);
    }
#endif
    default:
        return nullptr;
    }
}

Result apply_ipv4(const Ipv4& ipv4) {
    dns_servers[0].store(ipv4.dns1);
    dns_servers[1].store(ipv4.dns2);
    if (ipv4.address == 0U) {
        const auto result = esp_netif_dhcpc_start(netif);
        if (result == ESP_OK)
            has_address.store(false); // leaving static addressing: wait for the lease
        else if (result != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
            return Result::failed;
        apply_dns(); // a running lease keeps its address; a new one re-applies them
        return Result::success;
    }
    const auto stopped = esp_netif_dhcpc_stop(netif);
    if (stopped != ESP_OK && stopped != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
        return Result::failed;
    esp_netif_ip_info_t ip{};
    ip.ip.addr = ipv4.address;
    ip.gw.addr = ipv4.gateway;
    ip.netmask.addr = ipv4.netmask;
    if (esp_netif_set_ip_info(netif, &ip) != ESP_OK)
        return Result::failed;
    apply_dns();
    has_address.store(true); // usable only together with the link
    return Result::success;
}
} // namespace

bool supported(const Phy phy) {
    switch (phy) {
    case Phy::dm9051:
        return kDm9051;
    case Phy::w5500:
        return kW5500;
    case Phy::ksz8851:
        return kKsz8851;
    default:
        return kEmac && static_cast<std::uint8_t>(phy) < kPhyCount;
    }
}

Result start(const Config& config, const Ipv4& ipv4, const std::string_view hostname) {
    if (handle != nullptr)
        return Result::success;
    if (!supported(config.phy))
        return Result::unavailable;
    if (uses_spi(config.phy) ? !config.mdc.valid() : !config.mdc.valid() && !config.mdio.valid())
        return Result::invalid_argument;
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = config.address;
    phy_config.reset_gpio_num = gpio(config.power);
    esp_eth_mac_t* mac{};
    if (uses_spi(config.phy)) {
        AddLog(LogLevel::info, "ETH: Ethernet using SPI (bus%u)", unsigned(config.spi_bus) + 1U);
        mac = new_spi_mac(config, mac_config);
    } else {
#if CONFIG_ETH_USE_ESP32_EMAC
        AddLog(LogLevel::info, "ETH: Ethernet using RMII");
        mac = new_emac(config, mac_config);
#endif
    }
    esp_eth_phy_t* phy = mac != nullptr ? new_phy(config, phy_config) : nullptr;
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    if (mac == nullptr || phy == nullptr ||
        esp_eth_driver_install(&eth_config, &handle) != ESP_OK) {
        AddLog(LogLevel::error, "ETH: Bad EthType %u or init error", unsigned(config.phy));
        if (phy != nullptr)
            phy->del(phy);
        if (mac != nullptr)
            mac->del(mac); // Removes the controller's SPI device before the lease goes.
        if (spi_lease.valid())
            (void)spi_host::release(spi_lease);
        handle = nullptr;
        return Result::failed;
    }
    if (uses_spi(config.phy)) { // the controllers have no MAC of their own
        std::uint8_t address[6]{};
        if (esp_read_mac(address, ESP_MAC_ETH) == ESP_OK)
            (void)esp_eth_ioctl(handle, ETH_CMD_S_MAC_ADDR, address);
    }
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    netif = esp_netif_new(&netif_config);
    if (netif == nullptr || esp_netif_attach(netif, esp_eth_new_netif_glue(handle)) != ESP_OK ||
        esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &event, nullptr) != ESP_OK ||
        esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &event, nullptr) != ESP_OK ||
        esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, &event, nullptr) != ESP_OK) {
        AddLog(LogLevel::error, "ETH: interface set-up failed");
        return Result::failed;
    }
    // Tasmota names the wired interface <hostname>-eth.
    char name[64]{};
    std::snprintf(name, sizeof(name), "%.*s-eth",
        int(hostname.size() < 59U ? hostname.size() : 59U), hostname.data());
    (void)esp_netif_set_hostname(netif, name);
    // The interface starts its DHCP client by itself; each lease then applies the DNS overrides.
    dns_servers[0].store(ipv4.dns1);
    dns_servers[1].store(ipv4.dns2);
    if (ipv4.address != 0U)
        (void)apply_ipv4(ipv4);
    return esp_eth_start(handle) == ESP_OK ? Result::success : Result::failed;
}

Result set_ipv4(const Ipv4& ipv4) {
    return netif == nullptr ? Result::unavailable : apply_ipv4(ipv4);
}

bool connected() {
    return link_up.load() && has_address.load();
}

NetworkInfo info() {
    NetworkInfo result{};
    if (netif == nullptr)
        return result;
    const char* hostname{};
    if (esp_netif_get_hostname(netif, &hostname) == ESP_OK && hostname != nullptr)
        std::snprintf(result.hostname, sizeof(result.hostname), "%s", hostname);
    std::uint8_t mac[6]{};
    if (esp_eth_ioctl(handle, ETH_CMD_G_MAC_ADDR, mac) == ESP_OK)
        format_mac(result.mac, mac);
    esp_netif_ip_info_t ip{};
    if (connected())
        (void)esp_netif_get_ip_info(netif, &ip);
    format_ipv4(result.address, &ip.ip.addr);
    format_ipv4(result.gateway, &ip.gw.addr);
    format_ipv4(result.subnet, &ip.netmask.addr);
    for (int index = 0; index < 2; ++index) {
        esp_netif_dns_info_t dns{};
        if (esp_netif_get_dns_info(netif, static_cast<esp_netif_dns_type_t>(index), &dns) ==
                ESP_OK &&
            dns.ip.type == ESP_IPADDR_TYPE_V4)
            format_ipv4(index == 0 ? result.dns1 : result.dns2, &dns.ip.u_addr.ip4.addr);
    }
    return result;
}

} // namespace newmota::platform::ethernet
