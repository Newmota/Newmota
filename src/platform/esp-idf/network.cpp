/*
  network.cpp - ESP-IDF network implementation for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/network.h"
#include "network_internal.h"
#include "newmota_features.h"
#if defined(NEWMOTA_IMAGE_APPLICATION)
#include "newmota/log.h"
#include <memory>
#include <new>
#endif

#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#if USE_ESP_HOSTED
#include "newmota/platform/hosted.h"
#endif

#include <algorithm>
#include <atomic>
#include <cstring>

namespace newmota::platform {
namespace {

std::atomic<WifiState> state{WifiState::disabled};
std::atomic<bool> driver_running{};
std::atomic<bool> auth_failed{};
std::atomic<AccessPointState> ap_state{AccessPointState::disabled};
std::atomic<bool> scan_active{};
std::atomic<bool> scan_done{};
#if !defined(NEWMOTA_IMAGE_APPLICATION)
std::array<WifiScanRecord, kMaximumWifiScanResults> scan_results{};
std::array<wifi_ap_record_t, kMaximumWifiScanResults> native_scan_results{};
std::size_t scan_result_count{};
#endif
esp_netif_t* station = nullptr;
esp_netif_t* access_point = nullptr;
std::array<char, 64> configured_hostname{};

/** @brief Maps ESP-IDF authentication modes to portable scan values. */
WifiEncryption encryption(const wifi_auth_mode_t mode) {
    switch (mode) {
    case WIFI_AUTH_OPEN:
        return WifiEncryption::open;
    case WIFI_AUTH_WEP:
        return WifiEncryption::wep;
    case WIFI_AUTH_WPA_PSK:
        return WifiEncryption::wpa_psk;
    case WIFI_AUTH_WPA2_PSK:
        return WifiEncryption::wpa2_psk;
    case WIFI_AUTH_WPA_WPA2_PSK:
        return WifiEncryption::wpa_wpa2_psk;
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return WifiEncryption::wpa2_enterprise;
    case WIFI_AUTH_WPA3_PSK:
        return WifiEncryption::wpa3_psk;
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return WifiEncryption::wpa2_wpa3_psk;
    case WIFI_AUTH_WAPI_PSK:
        return WifiEncryption::wapi_psk;
    default:
        return WifiEncryption::unknown;
    }
}

/** @brief Converts ESP-IDF station events into the portable link state. */
void wifi_event(void*, const esp_event_base_t base, const std::int32_t id, void* const data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        driver_running.store(true);
        state.store(WifiState::idle);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) {
        driver_running.store(false);
        state.store(WifiState::disabled);
#if USE_MATTER
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        (void)esp_netif_create_ip6_linklocal(station);
#endif
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // A wrong WPA2 password has no reason of its own: the handshake just times out.
        switch (static_cast<const wifi_event_sta_disconnected_t*>(data)->reason) {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_MIC_FAILURE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            auth_failed.store(true);
            break;
        default:
            break;
        }
        state.store(WifiState::idle);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        scan_done.store(true, std::memory_order_release);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        state.store(WifiState::connected);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        ap_state.store(AccessPointState::active);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STOP) {
        ap_state.store(AccessPointState::idle);
    }
}

/** @brief Copies a bounded string into one ESP-IDF byte field. */
template <std::size_t N>
void copy_field(std::uint8_t (&destination)[N], const std::string_view source) {
    const auto count = std::min(source.size(), N);
    std::memcpy(destination, source.data(), count);
    if (count < N) {
        destination[count] = 0;
    }
}

} // namespace

/** @brief Initializes ESP-IDF networking and the AP-capable Wi-Fi radio. */
NetworkResult initialize_wifi() {
    driver_running.store(false);
    state.store(WifiState::initializing);
    ap_state.store(AccessPointState::initializing);
    if (nvs_flash_init() != ESP_OK) {
        state.store(WifiState::disabled);
        return NetworkResult::failed;
    }
    const auto netif_result = esp_netif_init();
    if (netif_result != ESP_OK && netif_result != ESP_ERR_INVALID_STATE) {
        state.store(WifiState::disabled);
        return NetworkResult::failed;
    }
    const auto loop_result = esp_event_loop_create_default();
    if (loop_result != ESP_OK && loop_result != ESP_ERR_INVALID_STATE) {
        state.store(WifiState::disabled);
        return NetworkResult::failed;
    }
#if USE_ESP_HOSTED
    // Every esp_wifi call below is an RPC to the companion.
    if (hosted::start() != hosted::Result::success) {
        state.store(WifiState::disabled);
        ap_state.store(AccessPointState::disabled);
        return NetworkResult::unavailable;
    }
#endif
    station = esp_netif_create_default_wifi_sta();
    access_point = esp_netif_create_default_wifi_ap();
    if (station == nullptr || access_point == nullptr) {
        state.store(WifiState::disabled);
        ap_state.store(AccessPointState::disabled);
        return NetworkResult::failed;
    }
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    config.nvs_enable = false;
    if (esp_wifi_init(&config) != ESP_OK ||
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event, nullptr) != ESP_OK ||
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event, nullptr) != ESP_OK ||
        esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ||
        esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_start() != ESP_OK) {
        driver_running.store(false);
        state.store(WifiState::disabled);
        ap_state.store(AccessPointState::disabled);
        return NetworkResult::failed;
    }
#if CONFIG_SOC_WIFI_SUPPORT_5G
    // Tasmota's Arduino core selects both bands; on failure the SDK's own band mode stays.
    (void)esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
#endif
    ap_state.store(AccessPointState::idle);
    return NetworkResult::success;
}

/** @brief Bluetooth coexistence needs modem sleep, so builds with Bluetooth keep it on. */
NetworkResult set_wifi_power_save(const bool enabled) {
#if CONFIG_BT_ENABLED
    return enabled ? NetworkResult::success : NetworkResult::unavailable;
#else
    return esp_wifi_set_ps(enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE) == ESP_OK
               ? NetworkResult::success
               : NetworkResult::failed;
#endif
}

/** @brief Tasmota's per-chip limits from include/tasmota.h. */
WifiTxPowerProfile wifi_tx_power_profile() {
#if CONFIG_IDF_TARGET_ESP32S2
    return {195, 150, 130, -880, -750, -720};
#elif CONFIG_IDF_TARGET_ESP32S3
    return {210, 190, 185, -880, -760, -720};
#elif CONFIG_IDF_TARGET_ESP32C2 || CONFIG_IDF_TARGET_ESP32C3
    return {210, 190, 185, -880, -760, -730};
#else
    return {195, 160, 140, -880, -750, -700};
#endif
}

/** @brief The SDK takes quarter dBm in 8..84 and rounds down to its supported levels. */
NetworkResult set_wifi_tx_power(const std::uint8_t tenths_dbm) {
    const int quarter_dbm = std::clamp(tenths_dbm * 4 / 10, 8, 84);
    return esp_wifi_set_max_tx_power(static_cast<std::int8_t>(quarter_dbm)) == ESP_OK
               ? NetworkResult::success
               : NetworkResult::failed;
}

/** @brief ESP-IDF supports concurrent APSTA operation on the configured radio. */
WifiCapabilities wifi_capabilities() {
#if CONFIG_IDF_TARGET_ESP32C5
    // Web setup scans every channel; on the ESP32-C5 a WPA2 join on 5 GHz took about 24 s.
    return {true, 30'000};
#else
    return {true};
#endif
}

/** @brief Replaces the RAM-only station credentials and starts connecting. */
NetworkResult connect_wifi(
    const std::string_view ssid, const std::string_view password, const WifiJoin join) {
    if (ssid.empty() || ssid.size() > kMaximumWifiSsidLength || password.size() > 64) {
        return NetworkResult::failed;
    }
    if (state.load() == WifiState::initializing) {
        return NetworkResult::busy;
    }
    wifi_config_t config{};
    copy_field(config.sta.ssid, ssid);
    copy_field(config.sta.password, password);
    // A fast scan stops at the first matching access point; sort_method applies to full scans.
    config.sta.scan_method = join == WifiJoin::first_found ? WIFI_FAST_SCAN : WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    (void)esp_wifi_disconnect();
    if (esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK) {
        return NetworkResult::failed;
    }
    auth_failed.store(false);
    state.store(WifiState::connecting);
    if (esp_wifi_connect() != ESP_OK) {
        state.store(WifiState::idle);
        return NetworkResult::failed;
    }
    return NetworkResult::success;
}

bool wifi_auth_failed() {
    return auth_failed.load();
}

/** @brief Starts a non-blocking ESP-IDF access-point scan. */
NetworkResult wifi_scan_start() {
    const auto station_state = state.load();
    if (station_state == WifiState::disabled)
        return NetworkResult::unavailable;
    if (station_state == WifiState::connecting)
        return NetworkResult::busy;
    bool expected = false;
    if (!scan_active.compare_exchange_strong(expected, true)) {
        return NetworkResult::busy;
    }
#if !defined(NEWMOTA_IMAGE_APPLICATION)
    scan_result_count = 0U;
#endif
    scan_done.store(false, std::memory_order_release);
    wifi_scan_config_t config{};
    const auto result = esp_wifi_scan_start(&config, false);
    if (result != ESP_OK) {
        scan_active.store(false, std::memory_order_release);
        return result == ESP_ERR_WIFI_STATE ? NetworkResult::busy : NetworkResult::failed;
    }
    return NetworkResult::success;
}

/** @brief Copies ESP-IDF records only after its scan-done event. */
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

#if defined(NEWMOTA_IMAGE_APPLICATION)
    const auto native_capacity = std::min(capacity, kMaximumWifiScanResults);
    if (native_capacity == 0U) {
        const auto cleared = esp_wifi_clear_ap_list();
        scan_active.store(false, std::memory_order_release);
        return cleared == ESP_OK ? NetworkResult::success : NetworkResult::failed;
    }
    // The bulk getter preserves the SDK's RSSI ordering and frees its complete AP list.
    // Allocate only after completion, and retain no native records between scans.
    const auto native_records =
        std::unique_ptr<wifi_ap_record_t[]>{new (std::nothrow) wifi_ap_record_t[native_capacity]{}};
    if (!native_records) {
        AddLog(LogLevel::error, "WIF: scan workspace allocation failed");
        (void)esp_wifi_clear_ap_list();
        scan_active.store(false, std::memory_order_release);
        return NetworkResult::failed;
    }
    auto native_count = static_cast<std::uint16_t>(native_capacity);
    if (esp_wifi_scan_get_ap_records(&native_count, native_records.get()) != ESP_OK) {
        (void)esp_wifi_clear_ap_list();
        scan_active.store(false, std::memory_order_release);
        return NetworkResult::failed;
    }
    count = std::min<std::size_t>(native_count, native_capacity);
    for (std::size_t index = 0U; index < count; ++index) {
        const auto& source = native_records[index];
        auto& target = results[index];
        std::memcpy(target.ssid.data(), source.ssid, target.ssid.size());
        target.ssid_length = 0U;
        while (target.ssid_length < target.ssid.size() && target.ssid[target.ssid_length] != '\0')
            ++target.ssid_length;
        std::memcpy(target.bssid.data(), source.bssid, target.bssid.size());
        target.channel = source.primary;
        target.rssi = source.rssi;
        target.encryption = encryption(source.authmode);
    }
#else
    std::uint16_t native_count = static_cast<std::uint16_t>(native_scan_results.size());
    if (esp_wifi_scan_get_ap_records(&native_count, native_scan_results.data()) != ESP_OK) {
        scan_active.store(false, std::memory_order_release);
        return NetworkResult::failed;
    }
    scan_result_count = std::min<std::size_t>(native_count, native_scan_results.size());
    for (std::size_t index = 0; index < scan_result_count; ++index) {
        const auto& source = native_scan_results[index];
        auto& target = scan_results[index];
        std::memcpy(target.ssid.data(), source.ssid, target.ssid.size());
        target.ssid_length = 0U;
        while (target.ssid_length < target.ssid.size() && target.ssid[target.ssid_length] != '\0') {
            ++target.ssid_length;
        }
        std::memcpy(target.bssid.data(), source.bssid, target.bssid.size());
        target.channel = source.primary;
        target.rssi = source.rssi;
        target.encryption = encryption(source.authmode);
    }
    count = std::min(scan_result_count, capacity);
    if (count != 0U)
        std::copy_n(scan_results.begin(), count, results);
#endif
    scan_active.store(false, std::memory_order_release);
    return NetworkResult::success;
}

/** @brief Stops an ESP-IDF scan after the portable request deadline. */
NetworkResult wifi_scan_cancel() {
    if (!scan_active.load(std::memory_order_acquire)) {
        return NetworkResult::success;
    }
    const auto result = esp_wifi_scan_stop();
    if (result != ESP_OK)
        return NetworkResult::failed;
    scan_done.store(true, std::memory_order_release);
    return NetworkResult::success;
}

/** @brief Returns the latest state reported by ESP-IDF events. */
WifiState wifi_state() {
    return state.load();
}

bool wifi_driver_running() {
    return driver_running.load();
}

/** @brief Returns both interface states without collapsing AP into station. */
WifiSnapshot wifi_snapshot() {
    const auto station_state = state.load();
    const auto access_point_state = ap_state.load();
    WifiMode mode = WifiMode::off;
    const bool station_enabled = station_state != WifiState::disabled;
    const bool access_point_enabled = access_point_state == AccessPointState::initializing ||
                                      access_point_state == AccessPointState::active;
    if (station_enabled && access_point_enabled) {
        mode = WifiMode::station_access_point;
    } else if (access_point_enabled) {
        mode = WifiMode::access_point;
    } else if (station_enabled) {
        mode = WifiMode::station;
    }
    return {mode, station_state, access_point_state};
}

/** @brief Returns the station MAC identity used by default names. */
std::uint32_t device_id() {
    std::uint8_t mac[6]{};
#if CONFIG_SOC_WIFI_SUPPORTED
    constexpr auto kIdentity = ESP_MAC_WIFI_STA;
#else
    constexpr auto kIdentity = ESP_MAC_BASE; // the station MAC belongs to the companion
#endif
    return esp_read_mac(mac, kIdentity) == ESP_OK ? device_id_from_mac(mac) : 0U;
}

/** @brief Applies one hostname to both ESP-NETIF interfaces. */
NetworkResult set_hostname(const std::string_view hostname) {
    if (station == nullptr || hostname.empty() || hostname.size() >= kMaximumWifiSsidLength) {
        return station == nullptr ? NetworkResult::unavailable : NetworkResult::failed;
    }
    configured_hostname.fill('\0');
    std::memcpy(configured_hostname.data(), hostname.data(), hostname.size());
    const auto station_result = esp_netif_set_hostname(station, configured_hostname.data());
    const auto ap_result = access_point == nullptr
                               ? ESP_OK
                               : esp_netif_set_hostname(access_point, configured_hostname.data());
    return station_result == ESP_OK && ap_result == ESP_OK ? NetworkResult::success
                                                           : NetworkResult::failed;
}

/** @brief Starts ESP-IDF's default AP netif and DHCP server. */
NetworkResult start_access_point(const std::string_view ssid, const std::string_view password) {
    if (access_point == nullptr || ssid.empty() || ssid.size() > kMaximumWifiSsidLength ||
        password.size() > 64 || (!password.empty() && password.size() < 8)) {
        return NetworkResult::failed;
    }
    if (state.load() == WifiState::initializing || state.load() == WifiState::disabled) {
        return NetworkResult::busy;
    }
    wifi_config_t config{};
    copy_field(config.ap.ssid, ssid);
    copy_field(config.ap.password, password);
    config.ap.ssid_len = static_cast<std::uint8_t>(ssid.size());
    config.ap.channel = 6;
    config.ap.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    config.ap.max_connection = 4;
    config.ap.pmf_cfg.required = false;
    config.ap.pmf_cfg.capable = true;
    ap_state.store(AccessPointState::initializing);
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_AP, &config) != ESP_OK) {
        ap_state.store(AccessPointState::idle);
        return NetworkResult::failed;
    }
    // APSTA is already running after initialize_wifi(); on SDKs that do not
    // emit AP_START for a config replacement, this is the completion point.
    ap_state.store(AccessPointState::active);
    return NetworkResult::success;
}

/** @brief Stops only the ESP-IDF AP interface and leaves STA initialized. */
NetworkResult stop_access_point() {
    if (access_point == nullptr)
        return NetworkResult::unavailable;
    if (ap_state.load() == AccessPointState::disabled ||
        ap_state.load() == AccessPointState::idle) {
        return NetworkResult::success;
    }
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
        return NetworkResult::failed;
    }
    ap_state.store(AccessPointState::idle);
    return NetworkResult::success;
}

/** @brief Reads station identity and IPv4/DNS values through ESP-NETIF APIs. */
NetworkInfo network_info() {
    NetworkInfo result{};
    if (station == nullptr) {
        return result;
    }
    const char* hostname = nullptr;
    if (esp_netif_get_hostname(station, &hostname) == ESP_OK && hostname != nullptr) {
        std::snprintf(result.hostname, sizeof(result.hostname), "%s", hostname);
    }
    std::uint8_t mac[6]{};
    if (esp_netif_get_mac(station, mac) == ESP_OK) {
        format_mac(result.mac, mac);
    }
    esp_netif_ip_info_t ip{};
    if (state.load() == WifiState::connected) {
        (void)esp_netif_get_ip_info(station, &ip);
    }
    format_ipv4(result.address, &ip.ip.addr);
    format_ipv4(result.gateway, &ip.gw.addr);
    format_ipv4(result.subnet, &ip.netmask.addr);
    for (int index = 0; index < 2; ++index) {
        esp_netif_dns_info_t dns{};
        if (esp_netif_get_dns_info(station, static_cast<esp_netif_dns_type_t>(index), &dns) ==
                ESP_OK &&
            dns.ip.type == ESP_IPADDR_TYPE_V4) {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(&dns.ip.u_addr.ip4.addr);
            std::snprintf(index == 0 ? result.dns1 : result.dns2, sizeof(result.dns1),
                "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
        }
    }
    return result;
}

/** @brief Reads the RSSI of the ESP-IDF station's current access point. */
std::optional<std::int8_t> station_rssi() {
    if (state.load() != WifiState::connected) {
        return std::nullopt;
    }
    wifi_ap_record_t record{};
    if (esp_wifi_sta_get_ap_info(&record) != ESP_OK) {
        return std::nullopt;
    }
    const int rssi = record.rssi;
    return static_cast<std::int8_t>(rssi < -127 ? -127 : rssi > 0 ? 0 : rssi);
}

bool wifi_link(WifiLink& link) {
    wifi_ap_record_t record{};
    if (state.load() != WifiState::connected || esp_wifi_sta_get_ap_info(&record) != ESP_OK)
        return false;
    std::memcpy(link.bssid, record.bssid, sizeof link.bssid);
    link.channel = record.primary;
    // Tasmota's names, indexed by wifi_phy_mode_t.
    static constexpr const char* kModes[] = {
        "low rate", "11b", "11g", "11a", "HT20", "HT40", "HE20", "VHT20"};
    wifi_phy_mode_t mode{};
    if (esp_wifi_sta_get_negotiated_phymode(&mode) == ESP_OK &&
        static_cast<std::size_t>(mode) < sizeof kModes / sizeof kModes[0])
        link.mode = kModes[mode];
    return true;
}

/** @brief Reads the ESP-IDF AP netif address and MAC. */
NetworkInfo access_point_info() {
    NetworkInfo result{};
    if (access_point == nullptr || ap_state.load() != AccessPointState::active) {
        return result;
    }
    const char* hostname = nullptr;
    if (esp_netif_get_hostname(access_point, &hostname) == ESP_OK && hostname != nullptr) {
        std::snprintf(result.hostname, sizeof(result.hostname), "%s", hostname);
    }
    std::uint8_t mac[6]{};
    if (esp_netif_get_mac(access_point, mac) == ESP_OK) {
        format_mac(result.mac, mac);
    }
    esp_netif_ip_info_t ip{};
    if (esp_netif_get_ip_info(access_point, &ip) == ESP_OK) {
        format_ipv4(result.address, &ip.ip.addr);
        format_ipv4(result.gateway, &ip.gw.addr);
        format_ipv4(result.subnet, &ip.netmask.addr);
    }
    return result;
}

} // namespace newmota::platform
