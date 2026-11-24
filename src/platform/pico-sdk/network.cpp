/*
  network.cpp - Raspberry Pi Pico W network implementation for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/network.h"

#include "dhcp_server.h"

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "lwip/dns.h"
#include "lwip/netif.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <new>

namespace newmota::platform {
namespace {

bool initialized = false;
bool auth_failed = false;
std::atomic<AccessPointState> ap_state{AccessPointState::disabled};
bool scan_active{};
using ScanResults = std::array<WifiScanRecord, kMaximumWifiScanResults>;
#if defined(NEWMOTA_IMAGE_SAFEBOOT)
// Safeboot keeps it static: its binary size matters more than its free heap.
ScanResults scan_results{};
#else
/** @brief Allocated by wifi_scan_start and freed by wifi_scan_poll, both under the CYW43
 * lock that the scan callback runs under. */
std::unique_ptr<ScanResults> scan_storage{};
#endif
std::size_t scan_result_count{};
std::array<char, 64> configured_hostname{};

/** @brief Maps CYW43 authentication modes to portable scan values. */
WifiEncryption encryption(const std::uint8_t auth) {
    switch (auth) {
    case 0U:
        return WifiEncryption::open;
    case 1U:
        return WifiEncryption::wep;
    case 2U:
        return WifiEncryption::wpa_psk;
    case 4U:
        return WifiEncryption::wpa2_psk;
    case 3U:
    case 5U:
    case 6U:
    case 7U:
        return WifiEncryption::wpa_wpa2_psk;
    default:
        return WifiEncryption::unknown;
    }
}

/** @brief Copies one CYW43 scan callback record into bounded storage. */
int scan_result_callback(void*, const cyw43_ev_scan_result_t* const source) {
    if (source == nullptr)
        return 0;
#if !defined(NEWMOTA_IMAGE_SAFEBOOT)
    if (!scan_storage)
        return 0;
    auto& scan_results = *scan_storage;
#endif
    std::size_t index = scan_result_count;
    if (index >= scan_results.size()) {
        index = 0U;
        for (std::size_t candidate = 1U; candidate < scan_results.size(); ++candidate) {
            if (scan_results[candidate].rssi < scan_results[index].rssi) {
                index = candidate;
            }
        }
        if (source->rssi <= scan_results[index].rssi)
            return 0;
    } else {
        ++scan_result_count;
    }
    auto& target = scan_results[index];
    const auto ssid_length = std::min<std::size_t>(source->ssid_len, target.ssid.size());
    std::memcpy(target.ssid.data(), source->ssid, ssid_length);
    target.ssid_length = static_cast<std::uint8_t>(ssid_length);
    std::memcpy(target.bssid.data(), source->bssid, target.bssid.size());
    target.channel = static_cast<std::uint8_t>(source->channel);
    target.rssi = static_cast<std::int8_t>(std::clamp<int>(source->rssi, -127, 0));
    target.encryption = encryption(source->auth_mode);
    return 0;
}

/** @brief Copies a bounded string into a null-terminated array. */
template <std::size_t N>
void copy_field(std::array<char, N>& destination, const std::string_view source) {
    const auto count = std::min(source.size(), N - 1);
    std::memcpy(destination.data(), source.data(), count);
    destination[count] = '\0';
}

/** @brief Applies the stored hostname while the CYW43 lwIP lock is held. */
void apply_configured_hostname_locked() {
    netif_set_hostname(&cyw43_state.netif[CYW43_ITF_STA], configured_hostname.data());
    netif_set_hostname(&cyw43_state.netif[CYW43_ITF_AP], configured_hostname.data());
}

} // namespace

/** @brief Initializes CYW43 and enables the station/AP-capable radio. */
NetworkResult initialize_wifi() {
    if (cyw43_arch_init() != 0) {
        return NetworkResult::failed;
    }
    cyw43_arch_enable_sta_mode();
    // The SDK default (PM2) sleeps after 200 ms without traffic, so a request after a short
    // pause waits seconds for the next wake-up. A mains-powered device answers immediately.
    (void)cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
    initialized = true;
    ap_state.store(AccessPointState::disabled);
    return NetworkResult::success;
}

/** @brief CYW43's power saving delays requests by seconds, so it stays off. */
NetworkResult set_wifi_power_save(const bool enabled) {
    return enabled ? NetworkResult::unavailable : NetworkResult::success;
}

/** @brief CYW43 exposes station and AP interfaces concurrently. */
WifiCapabilities wifi_capabilities() {
    return {true};
}

/** @brief Starts an asynchronous CYW43 station connection. */
NetworkResult connect_wifi(
    const std::string_view ssid, const std::string_view password, const WifiJoin /*join*/) {
    if (!initialized || ssid.empty() || ssid.size() > kMaximumWifiSsidLength ||
        password.size() > 64) {
        return NetworkResult::failed;
    }
    static std::array<char, 33> ssid_buffer{};
    static std::array<char, 65> password_buffer{};
    copy_field(ssid_buffer, ssid);
    copy_field(password_buffer, password);
    auth_failed = false;
    const auto authentication = password.empty() ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_AES_PSK;
    return cyw43_arch_wifi_connect_async(
               ssid_buffer.data(), password_buffer.data(), authentication) == 0
               ? NetworkResult::success
               : NetworkResult::failed;
}

/** @brief Starts CYW43's asynchronous scan and records callback results. */
NetworkResult wifi_scan_start() {
    if (!initialized)
        return NetworkResult::unavailable;
    if (wifi_state() == WifiState::connecting)
        return NetworkResult::busy;
    cyw43_arch_lwip_begin();
    if (scan_active || cyw43_wifi_scan_active(&cyw43_state)) {
        cyw43_arch_lwip_end();
        return NetworkResult::busy;
    }
    scan_result_count = 0U;
#if !defined(NEWMOTA_IMAGE_SAFEBOOT)
    scan_storage.reset(new (std::nothrow) ScanResults{});
    if (!scan_storage) {
        cyw43_arch_lwip_end();
        return NetworkResult::failed;
    }
#endif
    cyw43_wifi_scan_options_t options{};
    const auto result = cyw43_wifi_scan(&cyw43_state, &options, nullptr, &scan_result_callback);
    if (result == 0)
        scan_active = true;
#if !defined(NEWMOTA_IMAGE_SAFEBOOT)
    else
        scan_storage.reset();
#endif
    cyw43_arch_lwip_end();
    return result == 0 ? NetworkResult::success : NetworkResult::failed;
}

/** @brief Copies CYW43 results after its native scan-active flag clears. */
NetworkResult wifi_scan_poll(
    WifiScanRecord* const results, const std::size_t capacity, std::size_t& count) {
    count = 0U;
    if (results == nullptr && capacity != 0U)
        return NetworkResult::failed;
    if (!initialized)
        return NetworkResult::unavailable;
    cyw43_arch_lwip_begin();
    if (!scan_active) {
        cyw43_arch_lwip_end();
        return NetworkResult::unavailable;
    }
    if (cyw43_wifi_scan_active(&cyw43_state)) {
        cyw43_arch_lwip_end();
        return NetworkResult::busy;
    }
    count = std::min(scan_result_count, capacity);
#if defined(NEWMOTA_IMAGE_SAFEBOOT)
    if (count != 0U)
        std::copy_n(scan_results.begin(), count, results);
#else
    if (count != 0U)
        std::copy_n(scan_storage->begin(), count, results);
    scan_storage.reset();
#endif
    scan_active = false;
    cyw43_arch_lwip_end();
    return NetworkResult::success;
}

/** @brief CYW43 scans finish through their callback and have no abort API. */
NetworkResult wifi_scan_cancel() {
    if (!initialized)
        return NetworkResult::unavailable;
    cyw43_arch_lwip_begin();
    const bool active = scan_active || cyw43_wifi_scan_active(&cyw43_state);
    cyw43_arch_lwip_end();
    return active ? NetworkResult::unavailable : NetworkResult::success;
}

/** @brief Maps the CYW43 link and DHCP status to portable state. */
WifiState wifi_state() {
    if (!initialized) {
        return WifiState::disabled;
    }
    switch (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA)) {
    case CYW43_LINK_UP:
        return WifiState::connected;
    case CYW43_LINK_JOIN:
    case CYW43_LINK_NOIP:
        return WifiState::connecting;
    case CYW43_LINK_BADAUTH:
        // Kept, because the status does not outlive the link going down.
        auth_failed = true;
        return WifiState::idle;
    default:
        return WifiState::idle;
    }
}

bool wifi_auth_failed() {
    return auth_failed;
}

/** @brief Returns both CYW43 interface states without conflating AP and STA. */
WifiSnapshot wifi_snapshot() {
    const auto station_state = wifi_state();
    const auto access_point_state = ap_state.load();
    WifiMode mode = WifiMode::off;
    if (station_state != WifiState::disabled && access_point_state == AccessPointState::active) {
        mode = WifiMode::station_access_point;
    } else if (access_point_state == AccessPointState::active) {
        mode = WifiMode::access_point;
    } else if (station_state != WifiState::disabled) {
        mode = WifiMode::station;
    }
    return {mode, station_state, access_point_state};
}

/** @brief Returns the station MAC identity used by default names. */
std::uint32_t device_id() {
    if (!initialized)
        return 0U;
    std::uint8_t mac[6]{};
    cyw43_arch_lwip_begin();
    const auto result = cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac);
    cyw43_arch_lwip_end();
    return result == 0 ? device_id_from_mac(mac) : 0U;
}

/** @brief Applies the hostname to Pico W's station and AP netifs. */
NetworkResult set_hostname(const std::string_view hostname) {
    if (!initialized)
        return NetworkResult::unavailable;
    if (hostname.empty() || hostname.size() >= kMaximumWifiSsidLength) {
        return NetworkResult::failed;
    }
    configured_hostname.fill('\0');
    std::memcpy(configured_hostname.data(), hostname.data(), hostname.size());
    cyw43_arch_lwip_begin();
    apply_configured_hostname_locked();
    cyw43_arch_lwip_end();
    return NetworkResult::success;
}

/** @brief Enables the AP interface, its DHCP service, and the STA default route. */
NetworkResult start_access_point(const std::string_view ssid, const std::string_view password) {
    if (!initialized || ssid.empty() || ssid.size() > kMaximumWifiSsidLength ||
        password.size() > 64 || (!password.empty() && password.size() < 8)) {
        return NetworkResult::failed;
    }
    static std::array<char, 33> ap_ssid{};
    static std::array<char, 65> ap_password{};
    copy_field(ap_ssid, ssid);
    copy_field(ap_password, password);
    ap_state.store(AccessPointState::initializing);
    cyw43_arch_enable_ap_mode(ap_ssid.data(), password.empty() ? nullptr : ap_password.data(),
        password.empty() ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_AES_PSK);
    cyw43_arch_lwip_begin();
    apply_configured_hostname_locked();
    const bool dhcp_started = pico::start_dhcp_server(&cyw43_state.netif[CYW43_ITF_AP]);
    if (dhcp_started) {
        netif_set_default(&cyw43_state.netif[CYW43_ITF_STA]);
    }
    cyw43_arch_lwip_end();
    if (!dhcp_started) {
        cyw43_arch_disable_ap_mode();
        cyw43_arch_lwip_begin();
        netif_set_default(&cyw43_state.netif[CYW43_ITF_STA]);
        cyw43_arch_lwip_end();
        ap_state.store(AccessPointState::disabled);
        return NetworkResult::failed;
    }
    ap_state.store(AccessPointState::active);
    return NetworkResult::success;
}

/** @brief Disables the Pico CYW43 AP interface without tearing down STA. */
NetworkResult stop_access_point() {
    if (!initialized)
        return NetworkResult::unavailable;
    if (ap_state.load() != AccessPointState::active) {
        return NetworkResult::success;
    }
    cyw43_arch_lwip_begin();
    pico::stop_dhcp_server();
    netif_set_default(&cyw43_state.netif[CYW43_ITF_STA]);
    cyw43_arch_lwip_end();
    cyw43_arch_disable_ap_mode();
    ap_state.store(AccessPointState::disabled);
    return NetworkResult::success;
}

/** @brief Reads Pico W station addressing under the CYW43 lwIP lock. */
NetworkInfo network_info() {
    NetworkInfo result{};
    if (!initialized) {
        return result;
    }
    cyw43_arch_lwip_begin();
    auto& interface = cyw43_state.netif[CYW43_ITF_STA];
#if LWIP_NETIF_HOSTNAME
    if (interface.hostname != nullptr) {
        std::snprintf(result.hostname, sizeof(result.hostname), "%s", interface.hostname);
    }
#endif
    format_ipv4(result.address, &netif_ip4_addr(&interface)->addr);
    format_ipv4(result.gateway, &netif_ip4_gw(&interface)->addr);
    format_ipv4(result.subnet, &netif_ip4_netmask(&interface)->addr);
    format_mac(result.mac, interface.hwaddr);
    const auto* dns = dns_getserver(0);
    if (dns != nullptr && IP_IS_V4(dns)) {
        format_ipv4(result.dns1, &ip_2_ip4(dns)->addr);
    }
#if DNS_MAX_SERVERS > 1
    dns = dns_getserver(1);
    if (dns != nullptr && IP_IS_V4(dns)) {
        format_ipv4(result.dns2, &ip_2_ip4(dns)->addr);
    }
#endif
    cyw43_arch_lwip_end();
    return result;
}

/** @brief Reads the RSSI of CYW43's associated station. */
std::optional<std::int8_t> station_rssi() {
    if (wifi_state() != WifiState::connected) {
        return std::nullopt;
    }
    std::int32_t rssi = 0;
    if (cyw43_wifi_get_rssi(&cyw43_state, &rssi) != 0) {
        return std::nullopt;
    }
    return static_cast<std::int8_t>(rssi < -127 ? -127 : rssi > 0 ? 0 : rssi);
}

bool wifi_link(WifiLink& link) {
    if (wifi_state() != WifiState::connected || cyw43_wifi_get_bssid(&cyw43_state, link.bssid) != 0)
        return false;
    // channel_info_t: hw_channel, target_channel, scan_channel.
    std::uint32_t channel[3]{};
    if (cyw43_ioctl(&cyw43_state, CYW43_IOCTL_GET_CHANNEL, sizeof channel,
            reinterpret_cast<std::uint8_t*>(channel), CYW43_ITF_STA) == 0 &&
        channel[0] <= 14U)
        link.channel = static_cast<std::uint8_t>(channel[0]);
    return true;
}

/** @brief Reads the Pico AP netif address, gateway, mask, and MAC. */
NetworkInfo access_point_info() {
    NetworkInfo result{};
    if (!initialized || ap_state.load() != AccessPointState::active) {
        return result;
    }
    cyw43_arch_lwip_begin();
    auto& interface = cyw43_state.netif[CYW43_ITF_AP];
#if LWIP_NETIF_HOSTNAME
    if (interface.hostname != nullptr) {
        std::snprintf(result.hostname, sizeof(result.hostname), "%s", interface.hostname);
    }
#endif
    format_ipv4(result.address, &netif_ip4_addr(&interface)->addr);
    format_ipv4(result.gateway, &netif_ip4_gw(&interface)->addr);
    format_ipv4(result.subnet, &netif_ip4_netmask(&interface)->addr);
    format_mac(result.mac, interface.hwaddr);
    cyw43_arch_lwip_end();
    return result;
}

} // namespace newmota::platform
