/*
  network.cpp - Realtek AmebaD network implementation for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/network.h"

extern "C" {
#include "FreeRTOS.h"
#include "lwip/err.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "dhcp/dhcps.h"
#include "lwip_netconf.h"
#include "task.h"
#include "wifi_conf.h"
#include "wifi_constants.h"
}

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <new>

namespace newmota::platform {
namespace {

constexpr std::uint16_t kConnectTaskStackDepth = 1024;
constexpr std::uint16_t kScanTaskStackDepth = 1024;
std::array<char, 33> station_ssid{};
std::array<char, 65> station_password{};
std::array<char, 33> access_point_ssid{};
std::array<char, 65> access_point_password{};
std::atomic<WifiState> state{WifiState::disabled};
std::atomic<AccessPointState> ap_state{AccessPointState::disabled};
std::atomic<bool> scan_active{};
std::atomic<bool> scan_done{};
std::atomic<bool> scan_start_done{};
std::atomic<NetworkResult> scan_start_result{NetworkResult::busy};
using ScanResults = std::array<WifiScanRecord, kMaximumWifiScanResults>;
#if defined(NEWMOTA_IMAGE_SAFEBOOT)
// Safeboot keeps it static: its binary size matters more than its free heap.
ScanResults scan_results{};
#else
/** @brief Allocated before the scan task starts; freed by wifi_scan_poll after scan_done or a
 * failed start, when no SDK callback can follow. */
std::unique_ptr<ScanResults> scan_storage{};
#endif
std::atomic<std::size_t> scan_result_count{};
std::array<char, 64> configured_hostname{};

/** @brief Maps AmebaD security values to portable scan values. */
WifiEncryption encryption(const rtw_security_t security) {
    switch (security) {
    case RTW_SECURITY_OPEN:
    case RTW_SECURITY_WPS_OPEN:
        return WifiEncryption::open;
    case RTW_SECURITY_WEP_PSK:
    case RTW_SECURITY_WEP_SHARED:
        return WifiEncryption::wep;
    case RTW_SECURITY_WPA_TKIP_PSK:
    case RTW_SECURITY_WPA_AES_PSK:
    case RTW_SECURITY_WPA_MIXED_PSK:
        return WifiEncryption::wpa_psk;
    case RTW_SECURITY_WPA2_AES_PSK:
    case RTW_SECURITY_WPA2_TKIP_PSK:
    case RTW_SECURITY_WPA2_MIXED_PSK:
        return WifiEncryption::wpa2_psk;
    case RTW_SECURITY_WPA_WPA2_TKIP_PSK:
    case RTW_SECURITY_WPA_WPA2_AES_PSK:
    case RTW_SECURITY_WPA_WPA2_MIXED_PSK:
        return WifiEncryption::wpa_wpa2_psk;
    case RTW_SECURITY_WPA2_TKIP_ENTERPRISE:
    case RTW_SECURITY_WPA2_AES_ENTERPRISE:
    case RTW_SECURITY_WPA2_MIXED_ENTERPRISE:
        return WifiEncryption::wpa2_enterprise;
    case RTW_SECURITY_WPA3_AES_PSK:
        return WifiEncryption::wpa3_psk;
    case RTW_SECURITY_WPA2_WPA3_MIXED:
        return WifiEncryption::wpa2_wpa3_psk;
    default:
        return WifiEncryption::unknown;
    }
}

/** @brief Copies one AmebaD callback result into the bounded scan cache. */
rtw_result_t scan_result_handler(rtw_scan_handler_result_t* const result) {
    if (result == nullptr)
        return RTW_ERROR;
    if (result->scan_complete) {
        scan_done.store(true, std::memory_order_release);
        return RTW_SUCCESS;
    }
#if !defined(NEWMOTA_IMAGE_SAFEBOOT)
    if (!scan_storage)
        return RTW_SUCCESS;
    auto& scan_results = *scan_storage;
#endif
    auto index = scan_result_count.load(std::memory_order_relaxed);
    if (index >= scan_results.size())
        return RTW_SUCCESS;
    const auto& source = result->ap_details;
    auto& target = scan_results[index];
    const auto ssid_length = std::min<std::size_t>(source.SSID.len, target.ssid.size());
    std::memcpy(target.ssid.data(), source.SSID.val, ssid_length);
    target.ssid_length = static_cast<std::uint8_t>(ssid_length);
    std::memcpy(target.bssid.data(), source.BSSID.octet, target.bssid.size());
    target.channel = static_cast<std::uint8_t>(source.channel);
    const auto signal = static_cast<int>(source.signal_strength);
    target.rssi = static_cast<std::int8_t>(signal < -127 ? -127 : signal > 0 ? 0 : signal);
    target.encryption = encryption(source.security);
    scan_result_count.store(index + 1U, std::memory_order_relaxed);
    return RTW_SUCCESS;
}

/** @brief Copies a bounded string into a null-terminated array. */
template <std::size_t N>
void copy_field(std::array<char, N>& destination, const std::string_view source) {
    const auto count = source.size() < N - 1 ? source.size() : N - 1;
    std::memcpy(destination.data(), source.data(), count);
    destination[count] = '\0';
}

/** @brief Applies the stored hostname to both AmebaD lwIP interfaces. */
void apply_configured_hostname() {
#if LWIP_NETIF_HOSTNAME
    netif_set_hostname(&xnetif[0], configured_hostname.data());
    netif_set_hostname(&xnetif[1], configured_hostname.data());
#endif
}

/** @brief Assigns Newmota's fixed setup address to the AmebaD AP netif. */
void configure_access_point_ipv4() {
    struct ip_addr address{};
    struct ip_addr netmask{};
    struct ip_addr gateway{};
    IP4_ADDR(&address, 192, 168, 4, 1);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gateway, 192, 168, 4, 1);
    netif_set_addr(&xnetif[1], &address, &netmask, &gateway);
}

/** @brief Joins the selected AP and acquires an address without blocking Newmota. */
void connect_task(void*) {
    const auto security =
        station_password[0] == '\0' ? RTW_SECURITY_OPEN : RTW_SECURITY_WPA2_AES_PSK;
    const auto result = wifi_connect(station_ssid.data(), security, station_password.data(),
        static_cast<int>(std::strlen(station_ssid.data())),
        static_cast<int>(std::strlen(station_password.data())), -1, nullptr);
    if (result == RTW_SUCCESS) {
        const auto dhcp_result = LwIP_DHCP(0, DHCP_START);
        state.store(dhcp_result == DHCP_ADDRESS_ASSIGNED ? WifiState::connected : WifiState::idle);
    } else {
        state.store(WifiState::idle);
    }
    vTaskDelete(nullptr);
}

/** @brief Keeps the vendor's multi-channel scan waits off the app task. */
void scan_task(void*) {
    const auto result = ap_state.load() == AccessPointState::active
                            ? wifi_scan_networks_mcc(&scan_result_handler, nullptr)
                            : wifi_scan_networks(&scan_result_handler, nullptr);
    scan_start_result.store(result == RTW_SUCCESS ? NetworkResult::success : NetworkResult::failed,
        std::memory_order_release);
    scan_start_done.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}

} // namespace

/**
 * @brief Enables and selects AmebaD's concurrent station/AP radio mode.
 *
 * The vendor boot task starts station mode before Newmota. In that case
 * wifi_on(STA_AP) returns "already running" without changing the mode, so the
 * adapter must explicitly promote the running radio before using both netifs.
 */
NetworkResult initialize_wifi() {
    if (wifi_on(RTW_MODE_STA_AP) < 0) {
        return NetworkResult::failed;
    }
    if (wifi_set_mode(RTW_MODE_STA_AP) < 0) {
        return NetworkResult::failed;
    }
    state.store(WifiState::idle);
    ap_state.store(AccessPointState::disabled);
    return NetworkResult::success;
}

/** @brief The SDK's default Wi-Fi power saving stays in place until it is measured. */
NetworkResult set_wifi_power_save(bool) {
    return NetworkResult::unavailable;
}

/** @brief AmebaD is initialized in the vendor's concurrent STA/AP mode. */
WifiCapabilities wifi_capabilities() {
    return {true};
}

/** @brief Starts an AmebaD connection worker for one credential slot. */
NetworkResult connect_wifi(
    const std::string_view ssid, const std::string_view password, const WifiJoin /*join*/) {
    if (ssid.empty() || ssid.size() > kMaximumWifiSsidLength || password.size() > 64) {
        return NetworkResult::failed;
    }
    if (state.load() == WifiState::connecting) {
        return NetworkResult::busy;
    }
    (void)wifi_disconnect();
    copy_field(station_ssid, ssid);
    copy_field(station_password, password);
    state.store(WifiState::connecting);
    if (xTaskCreate(connect_task, "wifi-connect", kConnectTaskStackDepth, nullptr,
            tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
        state.store(WifiState::idle);
        return NetworkResult::failed;
    }
    return NetworkResult::success;
}

/** @brief Starts the AmebaD SDK's asynchronous active scan. */
NetworkResult wifi_scan_start() {
    if (state.load() == WifiState::disabled)
        return NetworkResult::unavailable;
    if (state.load() == WifiState::connecting)
        return NetworkResult::busy;
    bool expected = false;
    if (!scan_active.compare_exchange_strong(expected, true)) {
        return NetworkResult::busy;
    }
    scan_result_count.store(0U, std::memory_order_relaxed);
    scan_done.store(false, std::memory_order_release);
    scan_start_done.store(false, std::memory_order_release);
    scan_start_result.store(NetworkResult::busy, std::memory_order_relaxed);
#if !defined(NEWMOTA_IMAGE_SAFEBOOT)
    scan_storage.reset(new (std::nothrow) ScanResults{});
    if (!scan_storage) {
        scan_active.store(false, std::memory_order_release);
        return NetworkResult::failed;
    }
#endif
    if (xTaskCreate(scan_task, "wifi-scan", kScanTaskStackDepth, nullptr, tskIDLE_PRIORITY + 1,
            nullptr) != pdPASS) {
#if !defined(NEWMOTA_IMAGE_SAFEBOOT)
        scan_storage.reset();
#endif
        scan_active.store(false, std::memory_order_release);
        return NetworkResult::failed;
    }
    return NetworkResult::success;
}

/** @brief Copies AmebaD scan callbacks once the SDK marks them complete. */
NetworkResult wifi_scan_poll(
    WifiScanRecord* const results, const std::size_t capacity, std::size_t& count) {
    count = 0U;
    if (!scan_active.load(std::memory_order_acquire)) {
        return NetworkResult::unavailable;
    }
    if (!scan_start_done.load(std::memory_order_acquire)) {
        return NetworkResult::busy;
    }
    if (scan_start_result.load(std::memory_order_acquire) != NetworkResult::success) {
#if !defined(NEWMOTA_IMAGE_SAFEBOOT)
        scan_storage.reset();
#endif
        scan_active.store(false, std::memory_order_release);
        return NetworkResult::failed;
    }
    if (!scan_done.load(std::memory_order_acquire))
        return NetworkResult::busy;
    if (results == nullptr && capacity != 0U)
        return NetworkResult::failed;
    count = std::min(scan_result_count.load(std::memory_order_relaxed), capacity);
#if defined(NEWMOTA_IMAGE_SAFEBOOT)
    for (std::size_t index = 0; index < count; ++index) {
        results[index] = scan_results[index];
    }
#else
    for (std::size_t index = 0; index < count; ++index) {
        results[index] = (*scan_storage)[index];
    }
    scan_storage.reset();
#endif
    scan_active.store(false, std::memory_order_release);
    return NetworkResult::success;
}

/** @brief The Ameba scan worker drains its SDK operation before reuse. */
NetworkResult wifi_scan_cancel() {
    return scan_active.load(std::memory_order_acquire) ? NetworkResult::unavailable
                                                       : NetworkResult::success;
}

/** @brief Returns the latest AmebaD station/DHCP state. */
WifiState wifi_state() {
    if (state.load() == WifiState::connected &&
        wifi_is_ready_to_transceive(RTW_STA_INTERFACE) != RTW_SUCCESS) {
        state.store(WifiState::idle);
    }
    return state.load();
}

/** @brief Returns AmebaD station and SoftAP lifecycle independently. */
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
    if (state.load() == WifiState::disabled)
        return 0U;
    return device_id_from_mac(LwIP_GetMAC(&xnetif[0]));
}

/** @brief Applies the hostname to AmebaD's station and AP netifs. */
NetworkResult set_hostname(const std::string_view hostname) {
    if (state.load() == WifiState::disabled)
        return NetworkResult::unavailable;
    if (hostname.empty() || hostname.size() >= kMaximumWifiSsidLength) {
        return NetworkResult::failed;
    }
    configured_hostname.fill('\0');
    std::memcpy(configured_hostname.data(), hostname.data(), hostname.size());
    apply_configured_hostname();
    return NetworkResult::success;
}

/** @brief Starts the AmebaD AP interface and its lwIP DHCP server. */
NetworkResult start_access_point(const std::string_view ssid, const std::string_view password) {
    if (ssid.empty() || ssid.size() > kMaximumWifiSsidLength || password.size() > 64 ||
        (!password.empty() && password.size() < 8)) {
        return NetworkResult::failed;
    }
    if (state.load() == WifiState::disabled || state.load() == WifiState::initializing) {
        return NetworkResult::busy;
    }
    copy_field(access_point_ssid, ssid);
    copy_field(access_point_password, password);
    const auto security = password.empty() ? RTW_SECURITY_OPEN : RTW_SECURITY_WPA2_AES_PSK;
    ap_state.store(AccessPointState::initializing);
    // stop_access_point() selects station-only mode; restore the AP interface
    // before the SDK chooses which native netif to start.
    if (wifi_set_mode(RTW_MODE_STA_AP) < 0) {
        ap_state.store(AccessPointState::disabled);
        return NetworkResult::failed;
    }
    if (wifi_start_ap(access_point_ssid.data(), security,
            password.empty() ? nullptr : access_point_password.data(),
            static_cast<int>(ssid.size()), static_cast<int>(password.size()), 6) < 0) {
        ap_state.store(AccessPointState::disabled);
        return NetworkResult::failed;
    }
    apply_configured_hostname();
    configure_access_point_ipv4();
    dhcps_init(&xnetif[1]);
    ap_state.store(AccessPointState::active);
    return NetworkResult::success;
}

/** @brief Stops AmebaD AP/DHCP while retaining the station radio. */
NetworkResult stop_access_point() {
    if (ap_state.load() != AccessPointState::active) {
        return NetworkResult::success;
    }
    dhcps_deinit();
    if (wifi_set_mode(RTW_MODE_STA) < 0) {
        return NetworkResult::failed;
    }
    ap_state.store(AccessPointState::disabled);
    return NetworkResult::success;
}

/** @brief Reads AmebaD station addressing from its lwIP interface. */
NetworkInfo network_info() {
    NetworkInfo result{};
    if (state.load() != WifiState::connected) {
        return result;
    }
    auto& interface = xnetif[0];
#if LWIP_NETIF_HOSTNAME
    if (interface.hostname != nullptr) {
        std::snprintf(result.hostname, sizeof(result.hostname), "%s", interface.hostname);
    }
#else
    std::memcpy(result.hostname, configured_hostname.data(), configured_hostname.size());
#endif
    format_ipv4(result.address, LwIP_GetIP(&interface));
    format_ipv4(result.gateway, LwIP_GetGW(&interface));
    format_ipv4(result.subnet, LwIP_GetMASK(&interface));
    format_mac(result.mac, LwIP_GetMAC(&interface));
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
    return result;
}

/** @brief Reads the RSSI of AmebaD's associated station. */
std::optional<std::int8_t> station_rssi() {
    if (state.load() != WifiState::connected) {
        return std::nullopt;
    }
    int rssi = 0;
    if (wifi_get_rssi(&rssi) < 0) {
        return std::nullopt;
    }
    return static_cast<std::int8_t>(rssi < -127 ? -127 : rssi > 0 ? 0 : rssi);
}

bool wifi_link(WifiLink& link) {
    if (state.load() != WifiState::connected || wifi_get_ap_bssid(link.bssid) < 0)
        return false;
    int channel = 0;
    if (wifi_get_channel(&channel) >= 0 && channel > 0 && channel <= UINT8_MAX)
        link.channel = static_cast<std::uint8_t>(channel);
    return true;
}

/** @brief Reads the AmebaD AP netif address and hardware identity. */
NetworkInfo access_point_info() {
    NetworkInfo result{};
    if (ap_state.load() != AccessPointState::active ||
        wifi_is_ready_to_transceive(RTW_AP_INTERFACE) != RTW_SUCCESS) {
        return result;
    }
    auto& interface = xnetif[1];
#if LWIP_NETIF_HOSTNAME
    if (interface.hostname != nullptr) {
        std::snprintf(result.hostname, sizeof(result.hostname), "%s", interface.hostname);
    }
#else
    std::memcpy(result.hostname, configured_hostname.data(), configured_hostname.size());
#endif
    format_ipv4(result.address, LwIP_GetIP(&interface));
    format_ipv4(result.gateway, LwIP_GetGW(&interface));
    format_ipv4(result.subnet, LwIP_GetMASK(&interface));
    format_mac(result.mac, LwIP_GetMAC(&interface));
    return result;
}

} // namespace newmota::platform
