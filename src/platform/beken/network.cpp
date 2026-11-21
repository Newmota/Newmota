/*
  network.cpp - Beken network implementation for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/network.h"

extern "C" {
#include "net.h"
#include "wlan_ui_pub.h"
#include "rw_msg_pub.h"
#include "mem_pub.h"
#if CFG_WPA_CTRL_IFACE
#include "wpa_ctrl.h"
#endif
int net_dhcp_hostname_set(char* hostname);
}

#include <algorithm>
#include <array>
#include <cstring>

namespace newmota::platform {
namespace {

bool connecting = false;
bool initialized = false;
bool station_role_started = false;
bool auth_failed = false;
AccessPointState ap_state = AccessPointState::disabled;
bool scan_active{};
bool scan_uses_ap{};
volatile bool scan_done{};
volatile bool scan_failed{};
#if CFG_WPA_CTRL_IFACE
bool scan_notifier_registered{};
#endif
std::array<char, 64> configured_hostname{};

/** @brief Receives Beken's asynchronous scan-completion notification. */
#if CFG_WPA_CTRL_IFACE
void scan_callback(void*, const int type, int) {
    if (type == WLAN_EVENT_SCAN_FAILED) {
        scan_failed = true;
        scan_done = true;
    } else if (type == WLAN_EVENT_SCAN_SUCCESS || type == WLAN_EVENT_SCAN_RESULTS) {
        scan_failed = false;
        scan_done = true;
    }
}

/** @brief Registers the WPA scan notifier once the BDK queue is ready. */
bool ensure_scan_notifier_registered() {
    if (scan_notifier_registered)
        return true;
    if (wlan_register_notifier(&scan_callback, nullptr) != kNoErr) {
        return false;
    }
    scan_notifier_registered = true;
    return true;
}
#else
void scan_callback(void*, unsigned char) {
    scan_done = true;
}
#endif

/** @brief Maps Beken security values to portable scan values. */
WifiEncryption encryption(const int security) {
    switch (security) {
    case BK_SECURITY_TYPE_NONE:
        return WifiEncryption::open;
    case BK_SECURITY_TYPE_WEP:
        return WifiEncryption::wep;
    case BK_SECURITY_TYPE_WPA_TKIP:
    case BK_SECURITY_TYPE_WPA_AES:
    case BK_SECURITY_TYPE_WPA_MIXED:
        return WifiEncryption::wpa_psk;
    case BK_SECURITY_TYPE_WPA2_TKIP:
    case BK_SECURITY_TYPE_WPA2_AES:
    case BK_SECURITY_TYPE_WPA2_MIXED:
        return WifiEncryption::wpa2_psk;
    case BK_SECURITY_TYPE_WPA3_SAE:
        return WifiEncryption::wpa3_psk;
    case BK_SECURITY_TYPE_WPA3_WPA2_MIXED:
        return WifiEncryption::wpa2_wpa3_psk;
    case BK_SECURITY_TYPE_EAP:
        return WifiEncryption::wpa2_enterprise;
    default:
        return WifiEncryption::unknown;
    }
}

/** @brief Copies a bounded string into a null-terminated SDK field. */
template <std::size_t N> void copy_field(char (&destination)[N], const std::string_view source) {
    const auto count = source.size() < N - 1 ? source.size() : N - 1;
    std::memcpy(destination, source.data(), count);
    destination[count] = '\0';
}

/** @brief Reapplies the stored hostname after Beken recreates its netif. */
NetworkResult apply_configured_hostname() {
    return net_dhcp_hostname_set(configured_hostname.data()) == 0 ? NetworkResult::success
                                                                  : NetworkResult::failed;
}

} // namespace

/** @brief Accepts station initialization because Beken starts it per connection. */
NetworkResult initialize_wifi() {
    connecting = false;
    initialized = false;
    station_role_started = false;
    ap_state = AccessPointState::disabled;
    scan_active = false;
    scan_uses_ap = false;
    scan_done = false;
    scan_failed = false;
#if CFG_WPA_CTRL_IFACE
    // The WPA task may not have created its command queue yet. Scan callback
    // registration is retried when a scan is requested; it must not prevent
    // normal station or SoftAP startup.
    (void)ensure_scan_notifier_registered();
#else
    bk_wlan_scan_ap_reg_cb(&scan_callback);
#endif
    initialized = true;
    return NetworkResult::success;
}

/** @brief The SDK's default Wi-Fi power saving stays in place until it is measured. */
NetworkResult set_wifi_power_save(bool) {
    return NetworkResult::unavailable;
}

/** @brief The current Beken adapter has no qualified concurrent STA/AP path. */
WifiCapabilities wifi_capabilities() {
    return {false};
}

/** @brief Configures a DHCP station and starts the Beken connection task. */
NetworkResult connect_wifi(const std::string_view ssid, const std::string_view password) {
    if (ssid.empty() || ssid.size() > kMaximumWifiSsidLength || password.size() > 64) {
        return NetworkResult::failed;
    }
    if (bk_wlan_ap_is_up() > 0 || ap_state == AccessPointState::active ||
        ap_state == AccessPointState::initializing) {
        const auto stopped = stop_access_point();
        if (stopped != NetworkResult::success)
            return stopped;
    }
    network_InitTypeDef_st config{};
    config.wifi_mode = BK_STATION;
    config.dhcp_mode = DHCP_CLIENT;
    config.wifi_retry_interval = 1000;
    copy_field(config.wifi_ssid, ssid);
    copy_field(config.wifi_key, password);
    auth_failed = false;
    if (bk_wlan_start_sta(&config) != kNoErr) {
        connecting = false;
        return NetworkResult::failed;
    }
    station_role_started = true;
    (void)apply_configured_hostname();
    connecting = true;
    return NetworkResult::success;
}

/** @brief Starts Beken's non-blocking scan in station or AP mode. */
NetworkResult wifi_scan_start() {
    if (!initialized)
        return NetworkResult::unavailable;
#if CFG_WPA_CTRL_IFACE
    if (!ensure_scan_notifier_registered())
        return NetworkResult::unavailable;
#endif
    if (wifi_state() == WifiState::connecting)
        return NetworkResult::busy;
    if (scan_active)
        return NetworkResult::busy;
    scan_active = true;
    scan_done = false;
    scan_failed = false;
    scan_uses_ap = bk_wlan_ap_is_up() > 0;
    bk_wlan_start_scan();
    if (!scan_uses_ap)
        station_role_started = true;
    return NetworkResult::success;
}

/** @brief Copies Beken scan results after its completion callback. */
NetworkResult wifi_scan_poll(
    WifiScanRecord* const results, const std::size_t capacity, std::size_t& count) {
    count = 0U;
    if (!scan_active)
        return NetworkResult::unavailable;
    if (!scan_done)
        return NetworkResult::busy;
    if (scan_failed) {
        scan_active = false;
        scan_uses_ap = false;
        return NetworkResult::failed;
    }
    if (results == nullptr && capacity != 0U)
        return NetworkResult::failed;

#if CFG_WPA_CTRL_IFACE
    // With WPA control enabled, Beken keeps STA and AP scan results in
    // different stores. bk_wlan_start_scan() uses the AP path whenever the
    // onboarding SoftAP is running, so read through the matching result API.
    ScanResult_adv native{};
    const int scan_result =
        scan_uses_ap ? wlan_ap_scan_result(&native) : wlan_sta_scan_result(&native);
    if (scan_result != 0 || native.ApNum < 0 || (native.ApNum > 0 && native.ApList == nullptr)) {
        if (native.ApList != nullptr)
            os_free(native.ApList);
        scan_active = false;
        scan_uses_ap = false;
        return NetworkResult::failed;
    }

    count = std::min({static_cast<std::size_t>(native.ApNum), kMaximumWifiScanResults, capacity});
    for (std::size_t index = 0; index < count; ++index) {
        auto& target = results[index];
        const auto& source = native.ApList[index];
        std::size_t ssid_length = 0U;
        while (ssid_length < target.ssid.size() && source.ssid[ssid_length] != '\0') {
            ++ssid_length;
        }
        std::memcpy(target.ssid.data(), source.ssid, ssid_length);
        target.ssid_length = static_cast<std::uint8_t>(ssid_length);
        std::memcpy(target.bssid.data(), source.bssid, target.bssid.size());
        target.channel = static_cast<std::uint8_t>(source.channel);
        target.rssi =
            static_cast<std::int8_t>(std::clamp(static_cast<int>(source.ApPower), -127, 0));
        target.encryption = encryption(source.security);
    }
    if (native.ApList != nullptr)
        os_free(native.ApList);
#else
    // The legacy non-WPA BDK stores scan results in the raw scan-result list.
    const auto available = static_cast<std::size_t>(bk_wlan_get_scan_ap_result_numbers());
    const auto requested = std::min(available, kMaximumWifiScanResults);
    std::array<SCAN_RST_ITEM_T, kMaximumWifiScanResults> native{};
    const auto copied =
        bk_wlan_get_scan_ap_result(native.data(), static_cast<unsigned char>(requested));
    if (copied < 0) {
        scan_active = false;
        scan_uses_ap = false;
        return NetworkResult::failed;
    }
    count = std::min({static_cast<std::size_t>(copied), requested, capacity});
    for (std::size_t index = 0; index < count; ++index) {
        auto& target = results[index];
        const auto& source = native[index];
        std::size_t ssid_length = 0U;
        while (ssid_length < target.ssid.size() && source.ssid[ssid_length] != '\0') {
            ++ssid_length;
        }
        std::memcpy(target.ssid.data(), source.ssid, ssid_length);
        target.ssid_length = static_cast<std::uint8_t>(ssid_length);
        std::memcpy(target.bssid.data(), source.bssid, target.bssid.size());
        target.channel = static_cast<std::uint8_t>(source.channel);
        target.rssi = static_cast<std::int8_t>(std::clamp(source.level, -127, 0));
        target.encryption = encryption(source.security);
    }
#endif
    scan_active = false;
    scan_uses_ap = false;
    return NetworkResult::success;
}

/** @brief Stops a Beken scan when the portable request reaches its deadline. */
NetworkResult wifi_scan_cancel() {
    if (!scan_active)
        return NetworkResult::success;
    if (scan_done)
        return NetworkResult::success;
    if (bk_wlan_stop_scan() != 0)
        return NetworkResult::failed;
    scan_done = true;
    return NetworkResult::success;
}

/** @brief Reports a credential refusal since the last connect; retries overwrite the status. */
bool wifi_auth_failed() {
    const auto station_status = mhdr_get_station_status();
    if (station_status == RW_EVT_STA_PASSWORD_WRONG || station_status == RW_EVT_STA_AUTH_FAILED)
        auth_failed = true;
    return auth_failed;
}

/** @brief Reports whether the Beken DHCP client has installed an address. */
WifiState wifi_state() {
    if (!initialized)
        return WifiState::disabled;
    if (sta_ip_is_start() != 0) {
        connecting = false;
        return WifiState::connected;
    }
    const auto station_status = mhdr_get_station_status();
    if (station_status != RW_EVT_STA_CONNECTING && station_status < RW_EVT_STA_CONNECTED) {
        connecting = false;
    }
    return connecting ? WifiState::connecting : WifiState::idle;
}

/** @brief Returns Beken station/AP lifecycle state independently. */
WifiSnapshot wifi_snapshot() {
    const auto station_state = wifi_state();
    if (bk_wlan_ap_is_up() > 0) {
        ap_state = AccessPointState::active;
    } else if (ap_state == AccessPointState::active) {
        ap_state = AccessPointState::disabled;
    }
    WifiMode mode = WifiMode::off;
    if (station_state != WifiState::disabled && ap_state == AccessPointState::active) {
        mode = WifiMode::station_access_point;
    } else if (ap_state == AccessPointState::active) {
        mode = WifiMode::access_point;
    } else if (station_state != WifiState::disabled) {
        mode = WifiMode::station;
    }
    return {mode, station_state, ap_state};
}

/** @brief Returns the station MAC identity used by default names. */
std::uint32_t device_id() {
    std::uint8_t mac[6]{};
    bk_wifi_get_station_mac_address(reinterpret_cast<char*>(mac));
    return device_id_from_mac(mac);
}

/** @brief Applies the hostname to Beken's DHCP station interface. */
NetworkResult set_hostname(const std::string_view hostname) {
    if (!initialized)
        return NetworkResult::unavailable;
    if (hostname.empty() || hostname.size() >= kMaximumWifiSsidLength) {
        return NetworkResult::failed;
    }
    configured_hostname.fill('\0');
    std::memcpy(configured_hostname.data(), hostname.data(), hostname.size());
    return apply_configured_hostname();
}

/** @brief Starts the Beken SoftAP and its DHCP server. */
NetworkResult start_access_point(const std::string_view ssid, const std::string_view password) {
    if (!initialized || ssid.empty() || ssid.size() > kMaximumWifiSsidLength ||
        password.size() > 64 || (!password.empty() && password.size() < 8)) {
        return NetworkResult::failed;
    }
    if (station_role_started) {
        if (bk_wlan_stop(BK_STATION) != kNoErr) {
            return NetworkResult::failed;
        }
        station_role_started = false;
    }
    connecting = false;
    network_InitTypeDef_st config{};
    config.wifi_mode = BK_SOFT_AP;
    config.dhcp_mode = DHCP_SERVER;
    config.wifi_retry_interval = 100;
    copy_field(config.wifi_ssid, ssid);
    copy_field(config.wifi_key, password);
    std::snprintf(config.local_ip_addr, sizeof(config.local_ip_addr), "%s", "192.168.4.1");
    std::snprintf(config.net_mask, sizeof(config.net_mask), "%s", "255.255.255.0");
    std::snprintf(config.gateway_ip_addr, sizeof(config.gateway_ip_addr), "%s", "192.168.4.1");
    std::snprintf(
        config.dns_server_ip_addr, sizeof(config.dns_server_ip_addr), "%s", "192.168.4.1");
    ap_state = AccessPointState::initializing;
    if (bk_wlan_start(&config) != kNoErr) {
        ap_state = AccessPointState::disabled;
        return NetworkResult::failed;
    }
    connecting = false;
    ap_state = AccessPointState::active;
    (void)apply_configured_hostname();
    return NetworkResult::success;
}

/** @brief Stops Beken SoftAP without touching station settings. */
NetworkResult stop_access_point() {
    if (ap_state != AccessPointState::active && ap_state != AccessPointState::initializing &&
        bk_wlan_ap_is_up() <= 0) {
        return NetworkResult::success;
    }
    if (bk_wlan_stop(BK_SOFT_AP) != kNoErr) {
        return NetworkResult::failed;
    }
    ap_state = AccessPointState::disabled;
    return NetworkResult::success;
}

/** @brief Reads the Beken station addressing available after DHCP. */
NetworkInfo network_info() {
    NetworkInfo result{};
    if (sta_ip_is_start() == 0) {
        return result;
    }
    IPStatusTypedef status{};
    if (bk_wlan_get_ip_status(&status, BK_STATION) != kNoErr) {
        return result;
    }
    std::snprintf(result.hostname, sizeof(result.hostname), "%s", configured_hostname.data());
    std::snprintf(result.address, sizeof(result.address), "%s", status.ip);
    std::snprintf(result.gateway, sizeof(result.gateway), "%s", status.gate);
    std::snprintf(result.subnet, sizeof(result.subnet), "%s", status.mask);
    std::snprintf(result.dns1, sizeof(result.dns1), "%s", status.dns);
    format_mac(result.mac, status.mac);
    return result;
}

/** @brief Reads the RSSI reported for Beken's active station link. */
std::optional<std::int8_t> station_rssi() {
    if (wifi_state() != WifiState::connected) {
        return std::nullopt;
    }
    LinkStatusTypeDef status{};
    if (bk_wlan_get_link_status(&status) != kNoErr) {
        return std::nullopt;
    }
    const int rssi = status.wifi_strength;
    return static_cast<std::int8_t>(rssi < -127 ? -127 : rssi > 0 ? 0 : rssi);
}

bool wifi_link(WifiLink& link) {
    LinkStatusTypeDef status{};
    if (wifi_state() != WifiState::connected || bk_wlan_get_link_status(&status) != kNoErr)
        return false;
    std::memcpy(link.bssid, status.bssid, sizeof link.bssid);
    link.channel = static_cast<std::uint8_t>(status.channel);
    return true;
}

/** @brief Reads the Beken SoftAP address and identity. */
NetworkInfo access_point_info() {
    NetworkInfo result{};
    if (ap_state != AccessPointState::active || bk_wlan_ap_is_up() <= 0) {
        return result;
    }
    IPStatusTypedef status{};
    if (bk_wlan_get_ip_status(&status, BK_SOFT_AP) != kNoErr) {
        return result;
    }
    std::snprintf(result.hostname, sizeof(result.hostname), "%s", configured_hostname.data());
    std::snprintf(result.address, sizeof(result.address), "%s", status.ip);
    std::snprintf(result.gateway, sizeof(result.gateway), "%s", status.gate);
    std::snprintf(result.subnet, sizeof(result.subnet), "%s", status.mask);
    std::snprintf(result.dns1, sizeof(result.dns1), "%s", status.dns);
    std::snprintf(result.mac, sizeof(result.mac), "%s", status.mac);
    return result;
}

} // namespace newmota::platform
