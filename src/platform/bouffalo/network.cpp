/*
  network.cpp - Bouffalo Wi-Fi4/Wi-Fi6 network implementation for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file network.cpp
 * @brief Wi-Fi4 and FHOST Wi-Fi6 network lifecycle adapters.
 */

#include "newmota/platform/network.h"
#include "radio.h"

extern "C" {
#include "FreeRTOS.h"
#include "async_event.h"
#include "bflb_sec_trng.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "task.h"
#include "wifi_mgmr_ext.h"
#if !defined(BL602)
#include "fhost.h"
#include "fhost_api.h"
#include "wifi_mgmr.h"
#include "lwip/dns.h"
#endif
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

/** @brief Creates the BL602 Wi-Fi firmware task. */
extern "C" void wifi_task_create(void);
/** @brief Initializes the BL602 Wi-Fi host interface. */
extern "C" int fhost_init(void);
/** @brief Starts the BL602 Wi-Fi manager task. */
extern "C" int wifi_mgmr_task_start(void);
#if defined(BL602)
/**
 * @brief Gates every Wi-Fi4 adapter print, including the firmware library's.
 *
 * The Wi-Fi manager prints SSIDs, passphrases, and PSKs through that adapter.
 */
extern "C" volatile bool sys_log_all_enable;
#endif

/** @brief Supplies lwIP with a hardware-backed random value. */
extern "C" int bl_rand(void) {
    return static_cast<int>(static_cast<unsigned long>(random()) & 0x7fffffffUL);
}

namespace newmota::platform {
namespace {

constexpr std::uint16_t kWifiInitTaskStackDepth = 1024;
std::array<char, 33> station_ssid{};
std::array<char, 65> station_password{};
std::array<char, 33> access_point_ssid{};
std::array<char, 65> access_point_password{};
std::atomic<WifiState> state{WifiState::disabled};
std::atomic<AccessPointState> ap_state{AccessPointState::disabled};
std::atomic<bool> scan_active{};
std::atomic<bool> scan_done{};
#if defined(NEWMOTA_IMAGE_SAFEBOOT)
// Safeboot keeps its copy static: its binary size matters more than its free heap.
std::array<WifiScanRecord, kMaximumWifiScanResults> scan_results{};
#else
/** @brief The caller's array during wifi_scan_poll; the SDK iterator calls back synchronously. */
WifiScanRecord* scan_target{};
std::size_t scan_target_capacity{};
#endif
std::uint32_t scan_result_count{};
/** @brief AP MAC from efuse; the BL602 getter rereads efuse (and switches the root clock)
 * on every call. 0 unread, 1 being read by one task, 2 ready. */
std::array<std::uint8_t, 6> access_point_mac{};
std::atomic<int> access_point_mac_state{0};

/** @brief Reads the AP MAC once; a caller racing the first read reads it itself. */
bool read_access_point_mac(std::uint8_t (&mac)[6]) {
    int expected = 0;
    if (access_point_mac_state.compare_exchange_strong(expected, 1)) {
        const bool read = wifi_mgmr_ap_mac_get(access_point_mac.data()) == 0;
        access_point_mac_state.store(read ? 2 : 0, std::memory_order_release);
    }
    if (access_point_mac_state.load(std::memory_order_acquire) != 2)
        return wifi_mgmr_ap_mac_get(mac) == 0;
    std::copy(access_point_mac.begin(), access_point_mac.end(), mac);
    return true;
}
#if defined(BL602)
wifi_interface_t access_point_interface = nullptr;
#else
using wifi_mgmr_ap_item_t = wifi_mgmr_scan_item_t;

/** @brief Resolves FHOST station/AP netifs without relying on Wi-Fi4 symbols. */
netif* wifi_mgmr_sta_netif_get() {
    return static_cast<netif*>(fhost_to_net_if(MGMR_VIF_STA));
}

netif* wifi_mgmr_ap_netif_get() {
    return static_cast<netif*>(fhost_to_net_if(MGMR_VIF_AP));
}
#endif
std::array<char, 64> configured_hostname{};

/** @brief Maps BL602 authentication modes to portable scan values. */
WifiEncryption encryption(const std::uint8_t auth) {
    switch (auth) {
    case WIFI_EVENT_BEACON_IND_AUTH_OPEN:
        return WifiEncryption::open;
    case WIFI_EVENT_BEACON_IND_AUTH_WEP:
        return WifiEncryption::wep;
    case WIFI_EVENT_BEACON_IND_AUTH_WPA_PSK:
        return WifiEncryption::wpa_psk;
    case WIFI_EVENT_BEACON_IND_AUTH_WPA2_PSK:
        return WifiEncryption::wpa2_psk;
    case WIFI_EVENT_BEACON_IND_AUTH_WPA_WPA2_PSK:
        return WifiEncryption::wpa_wpa2_psk;
    case WIFI_EVENT_BEACON_IND_AUTH_WPA_ENT:
        return WifiEncryption::wpa2_enterprise;
    case WIFI_EVENT_BEACON_IND_AUTH_WPA3_SAE:
        return WifiEncryption::wpa3_psk;
    case WIFI_EVENT_BEACON_IND_AUTH_WPA2_PSK_WPA3_SAE:
        return WifiEncryption::wpa2_wpa3_psk;
    default:
        return WifiEncryption::unknown;
    }
}

/** @brief Copies a bounded string into a null-terminated array. */
template <std::size_t N>
void copy_field(std::array<char, N>& destination, const std::string_view source) {
    const auto count = source.size() < N - 1 ? source.size() : N - 1;
    std::memcpy(destination.data(), source.data(), count);
    destination[count] = '\0';
}

/** @brief Applies the stored hostname to every BL602 interface that exists. */
NetworkResult apply_configured_hostname() {
    auto* const station = wifi_mgmr_sta_netif_get();
    auto* const access_point = wifi_mgmr_ap_netif_get();
    if (station == nullptr && access_point == nullptr) {
        return NetworkResult::unavailable;
    }
    if (station != nullptr) {
        netif_set_hostname(station, configured_hostname.data());
    }
    if (access_point != nullptr) {
        netif_set_hostname(access_point, configured_hostname.data());
    }
    return NetworkResult::success;
}

/** @brief Copies one result from BL602's bounded scan cache. */
#if defined(BL602)
void copy_scan_item(wifi_mgmr_ap_item_t*, std::uint32_t* count, wifi_mgmr_ap_item_t* const item) {
#else
void copy_scan_item(void*, void* argument, wifi_mgmr_ap_item_t* const item) {
    auto* const count = static_cast<std::uint32_t*>(argument);
#endif
#if defined(NEWMOTA_IMAGE_SAFEBOOT)
    if (count == nullptr || item == nullptr || *count >= kMaximumWifiScanResults) {
        return;
    }
    auto& result = scan_results[*count];
#else
    if (count == nullptr || item == nullptr || *count >= scan_target_capacity) {
        return;
    }
    auto& result = scan_target[*count];
    result = {};
#endif
    const auto ssid_length =
        std::min<std::uint32_t>(item->ssid_len, static_cast<std::uint32_t>(result.ssid.size()));
    std::memcpy(result.ssid.data(), item->ssid, ssid_length);
    result.ssid_length = static_cast<std::uint8_t>(ssid_length);
    std::memcpy(result.bssid.data(), item->bssid, result.bssid.size());
    result.channel = item->channel;
    result.rssi = item->rssi;
    result.encryption = encryption(item->auth);
    ++*count;
}

/** @brief Tracks the BL602 Wi-Fi manager and DHCP lifecycle. */
void wifi_event(async_input_event_t event, void*) {
    switch (event->code) {
    case CODE_WIFI_ON_INIT_DONE:
        (void)wifi_mgmr_task_start();
        break;
    case CODE_WIFI_ON_MGMR_DONE:
    case CODE_WIFI_ON_DISCONNECT:
        state.store(WifiState::idle);
        break;
    case CODE_WIFI_ON_CONNECTED:
        state.store(WifiState::connecting);
        break;
    case CODE_WIFI_ON_GOT_IP:
        state.store(WifiState::connected);
        break;
#if !defined(BL602)
    case CODE_WIFI_ON_AP_STARTED:
        ap_state.store(AccessPointState::active);
        break;
    case CODE_WIFI_ON_AP_STOPPED:
        ap_state.store(AccessPointState::disabled);
        break;
#endif
    case CODE_WIFI_ON_SCAN_DONE:
        scan_done.store(true, std::memory_order_release);
        break;
    default:
        break;
    }
}

/** @brief Boots the BL602 radio firmware after the scheduler is running. */
void wifi_init_task(void*) {
    (void)async_register_event_filter(EV_WIFI, &wifi_event, nullptr);
    wifi_task_create();
    (void)fhost_init();
    vTaskDelete(nullptr);
}

} // namespace

/** @brief Initializes RF, lwIP, and the BL602 station manager. */
NetworkResult initialize_wifi() {
#if defined(BL602)
    sys_log_all_enable = false;
#endif
    state.store(WifiState::initializing);
    ap_state.store(AccessPointState::disabled);
    if (!initialize_radio()) {
        state.store(WifiState::disabled);
        return NetworkResult::failed;
    }
    tcpip_init(nullptr, nullptr);
    if (xTaskCreate(wifi_init_task, "wifi-init", kWifiInitTaskStackDepth, nullptr, 10, nullptr) !=
        pdPASS) {
        state.store(WifiState::disabled);
        return NetworkResult::failed;
    }
    return NetworkResult::success;
}

/** @brief The SDK's default Wi-Fi power saving stays in place until it is measured. */
NetworkResult set_wifi_power_save(bool) {
    return NetworkResult::unavailable;
}

/** @brief BL602 exposes independent station and AP manager interfaces. */
WifiCapabilities wifi_capabilities() {
    return {true};
}

/** @brief Starts one BL602 station connection with DHCP enabled. */
NetworkResult connect_wifi(
    const std::string_view ssid, const std::string_view password, const WifiJoin /*join*/) {
    if (state.load() == WifiState::initializing) {
        return NetworkResult::busy;
    }
    if (ssid.empty() || ssid.size() > kMaximumWifiSsidLength || password.size() > 64) {
        return NetworkResult::failed;
    }
    copy_field(station_ssid, ssid);
    copy_field(station_password, password);
#if defined(BL602)
    auto* interface = static_cast<wifi_interface_t*>(wifi_mgmr_sta_enable());
    if (interface == nullptr)
        return NetworkResult::failed;
    (void)apply_configured_hostname();
    state.store(WifiState::connecting);
    if (wifi_mgmr_sta_connect(interface, station_ssid.data(),
            station_password[0] == '\0' ? nullptr : station_password.data(), nullptr, nullptr, 0,
            0) != 0) {
#else
    wifi_mgmr_sta_connect_params_t config{};
    std::memcpy(config.ssid, ssid.data(), ssid.size());
    std::memcpy(config.key, password.data(), password.size());
    config.ssid_len = ssid.size();
    config.key_len = password.size();
    config.use_dhcp = 1;
    (void)apply_configured_hostname();
    state.store(WifiState::connecting);
    if (wifi_mgmr_sta_connect(&config) != 0) {
#endif
        state.store(WifiState::idle);
        return NetworkResult::failed;
    }
    return NetworkResult::success;
}

/** @brief Starts a non-blocking BL602 Wi-Fi manager scan. */
NetworkResult wifi_scan_start() {
    if (state.load() == WifiState::initializing || state.load() == WifiState::disabled) {
        return NetworkResult::unavailable;
    }
    if (state.load() == WifiState::connecting)
        return NetworkResult::busy;
    bool expected = false;
    if (!scan_active.compare_exchange_strong(expected, true)) {
        return NetworkResult::busy;
    }
    scan_done.store(false, std::memory_order_release);
    scan_result_count = 0U;
#if defined(BL602)
    const int result = wifi_mgmr_sta_enable() == nullptr ? -1 : wifi_mgmr_scan(nullptr, nullptr);
#else
    wifi_mgmr_scan_params_t config{};
    const int result = wifi_mgmr_sta_scan(&config);
#endif
    if (result != 0) {
        scan_active.store(false, std::memory_order_release);
        return NetworkResult::failed;
    }
    return NetworkResult::success;
}

/** @brief Reads the BL602 scan list after its asynchronous completion event. */
NetworkResult wifi_scan_poll(
    WifiScanRecord* const results, const std::size_t capacity, std::size_t& count) {
    count = 0U;
    if (!scan_active.load(std::memory_order_acquire)) {
        return NetworkResult::unavailable;
    }
    if (!scan_done.load(std::memory_order_acquire))
        return NetworkResult::busy;
    if (results == nullptr && capacity != 0U)
        return NetworkResult::failed;

    scan_result_count = 0U;
#if !defined(NEWMOTA_IMAGE_SAFEBOOT)
    scan_target = results;
    scan_target_capacity = std::min(capacity, kMaximumWifiScanResults);
#endif
    const bool listed = wifi_mgmr_scan_ap_all(nullptr, &scan_result_count, &copy_scan_item) == 0;
#if !defined(NEWMOTA_IMAGE_SAFEBOOT)
    scan_target = nullptr;
    scan_target_capacity = 0U;
#endif
    if (!listed) {
        scan_active.store(false, std::memory_order_release);
        return NetworkResult::failed;
    }
    count = std::min(static_cast<std::size_t>(scan_result_count), capacity);
#if defined(NEWMOTA_IMAGE_SAFEBOOT)
    for (std::size_t index = 0; index < count; ++index) {
        results[index] = scan_results[index];
    }
#endif
    scan_active.store(false, std::memory_order_release);
    return NetworkResult::success;
}

/** @brief BL602 scan requests drain through their manager completion event. */
NetworkResult wifi_scan_cancel() {
    return scan_active.load(std::memory_order_acquire) ? NetworkResult::unavailable
                                                       : NetworkResult::success;
}

/** @brief Returns the latest BL602 station/DHCP event state. */
WifiState wifi_state() {
    return state.load();
}

/** @brief Returns BL602 station/AP manager state independently. */
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
    std::uint8_t mac[6]{};
    return wifi_mgmr_sta_mac_get(mac) == 0 ? device_id_from_mac(mac) : 0U;
}

/** @brief Applies the hostname to BL602's station and AP netifs. */
NetworkResult set_hostname(const std::string_view hostname) {
    if (hostname.empty() || hostname.size() >= kMaximumWifiSsidLength) {
        return NetworkResult::failed;
    }
    configured_hostname.fill('\0');
    std::memcpy(configured_hostname.data(), hostname.data(), hostname.size());
    return apply_configured_hostname();
}

/** @brief Starts BL602's AP manager, including its built-in DHCP service. */
NetworkResult start_access_point(const std::string_view ssid, const std::string_view password) {
    if (ssid.empty() || ssid.size() > kMaximumWifiSsidLength || password.size() > 64 ||
        (!password.empty() && password.size() < 8)) {
        return NetworkResult::failed;
    }
    if (state.load() == WifiState::initializing || state.load() == WifiState::disabled) {
        return NetworkResult::busy;
    }
    copy_field(access_point_ssid, ssid);
    copy_field(access_point_password, password);
#if defined(BL602)
    access_point_interface = wifi_mgmr_ap_enable();
    if (access_point_interface == nullptr) {
        return NetworkResult::failed;
    }
    ap_state.store(AccessPointState::initializing);
    const auto result = wifi_mgmr_ap_start(&access_point_interface, access_point_ssid.data(), 0,
        password.empty() ? nullptr : access_point_password.data(), 6);
#else
    wifi_mgmr_ap_params_t config{};
    config.ssid = access_point_ssid.data();
    config.key = password.empty() ? nullptr : access_point_password.data();
    config.channel = 6;
    // Without use_ipcfg the SDK assigns no AP address and never starts its DHCP server.
    config.use_ipcfg = true;
    config.use_dhcpd = true;
    config.ap_ipaddr = PP_HTONL(LWIP_MAKEU32(192, 168, 4, 1));
    config.ap_mask = PP_HTONL(LWIP_MAKEU32(255, 255, 255, 0));
    config.start = 2;
    config.limit = 10;
    config.bcn_mode = 2;
    ap_state.store(AccessPointState::initializing);
    const auto result = wifi_mgmr_ap_start(&config);
#endif
    if (result != 0) {
        ap_state.store(AccessPointState::disabled);
        return NetworkResult::failed;
    }
    (void)apply_configured_hostname();
#if defined(BL602)
    ap_state.store(AccessPointState::active);
#endif
    return NetworkResult::success;
}

/** @brief Stops BL602's AP manager while leaving its radio task alive. */
NetworkResult stop_access_point() {
    if (ap_state.load() != AccessPointState::active) {
        return NetworkResult::success;
    }
#if defined(BL602)
    const int result =
        access_point_interface == nullptr ? -1 : wifi_mgmr_ap_stop(&access_point_interface);
#else
    const int result = wifi_mgmr_ap_stop();
#endif
    if (result != 0) {
        return NetworkResult::failed;
    }
    ap_state.store(AccessPointState::disabled);
    return NetworkResult::success;
}

/** @brief Reads BL602 station addressing maintained by the Wi-Fi manager. */
NetworkInfo network_info() {
    NetworkInfo result{};
    if (state.load() != WifiState::connected) {
        return result;
    }
    std::snprintf(result.hostname, sizeof(result.hostname), "%s", configured_hostname.data());
    std::uint32_t address = 0;
    std::uint32_t gateway = 0;
    std::uint32_t subnet = 0;
    std::uint32_t dns1 = 0;
    std::uint32_t dns2 = 0;
    std::uint8_t mac[6]{};
#if defined(BL602)
    const int ip_result = wifi_mgmr_sta_ip_get(&address, &gateway, &subnet);
#else
    const int ip_result = wifi_sta_ip4_addr_get(&address, &subnet, &gateway, &dns1);
#endif
    if (ip_result == 0) {
        format_ipv4(result.address, &address);
        format_ipv4(result.gateway, &gateway);
        format_ipv4(result.subnet, &subnet);
    }
#if defined(BL602)
    if (wifi_mgmr_sta_dns_get(&dns1, &dns2) == 0) {
#else
    dns1 = ip4_addr_get_u32(ip_2_ip4(dns_getserver(0)));
    dns2 = ip4_addr_get_u32(ip_2_ip4(dns_getserver(1)));
    {
#endif
        format_ipv4(result.dns1, &dns1);
        format_ipv4(result.dns2, &dns2);
    }
    if (wifi_mgmr_sta_mac_get(mac) == 0) {
        format_mac(result.mac, mac);
    }
    return result;
}

/** @brief Reads the RSSI of BL602's current station connection. */
std::optional<std::int8_t> station_rssi() {
    if (state.load() != WifiState::connected) {
        return std::nullopt;
    }
    int rssi = 0;
#if defined(BL602)
    const int result = wifi_mgmr_rssi_get(&rssi);
#else
    const int result = wifi_mgmr_sta_rssi_get(&rssi);
#endif
    if (result != 0) {
        return std::nullopt;
    }
    return static_cast<std::int8_t>(rssi < -127 ? -127 : rssi > 0 ? 0 : rssi);
}

bool wifi_link(WifiLink& link) {
    if (state.load() != WifiState::connected)
        return false;
#if defined(BL602)
    wifi_mgmr_sta_connect_ind_stat_info_t info{};
    wifi_mgmr_sta_connect_ind_stat_get(&info);
    std::memcpy(link.bssid, info.bssid, sizeof link.bssid);
    link.channel = info.chan_id;
#else
    if (wifi_mgmr_sta_get_bssid(link.bssid) != 0)
        return false;
    std::uint8_t channel = 0;
    if (wifi_mgmr_sta_channel_get(&channel) == 0)
        link.channel = channel;
#endif
    return true;
}

/** @brief Reads BL602 AP addressing and its AP-side MAC. */
NetworkInfo access_point_info() {
    NetworkInfo result{};
    if (ap_state.load() != AccessPointState::active) {
        return result;
    }
    std::snprintf(result.hostname, sizeof(result.hostname), "%s", configured_hostname.data());
    std::uint32_t address = 0;
    std::uint32_t gateway = 0;
    std::uint32_t subnet = 0;
    std::uint8_t mac[6]{};
#if defined(BL602)
    const bool has_ip = wifi_mgmr_ap_ip_get(&address, &gateway, &subnet) == 0;
#else
    auto* const interface = wifi_mgmr_ap_netif_get();
    const bool has_ip = interface != nullptr;
    if (has_ip) {
        address = ip4_addr_get_u32(netif_ip4_addr(interface));
        gateway = ip4_addr_get_u32(netif_ip4_gw(interface));
        subnet = ip4_addr_get_u32(netif_ip4_netmask(interface));
    }
#endif
    if (has_ip) {
        format_ipv4(result.address, &address);
        format_ipv4(result.gateway, &gateway);
        format_ipv4(result.subnet, &subnet);
    }
    if (read_access_point_mac(mac)) {
        format_mac(result.mac, mac);
    }
    return result;
}

} // namespace newmota::platform
