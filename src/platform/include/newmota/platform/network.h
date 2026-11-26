/*
  network.h - portable network platform interface for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string_view>

namespace newmota::platform {

enum class NetworkResult : std::uint8_t {
    success,
    busy,
    unavailable,
    failed,
};

enum class WifiState : std::uint8_t {
    disabled,
    initializing,
    idle,
    connecting,
    connected,
};

/** @brief Lifecycle state of the device's SoftAP interface. */
enum class AccessPointState : std::uint8_t {
    disabled,
    initializing,
    idle,
    active,
};

/** @brief Native radio mode currently represented by the platform adapter. */
enum class WifiMode : std::uint8_t {
    off,
    station,
    access_point,
    station_access_point,
};

/** @brief Security family reported for a discovered access point. */
enum class WifiEncryption : std::uint8_t {
    unknown,
    open,
    wep,
    wpa_psk,
    wpa2_psk,
    wpa_wpa2_psk,
    wpa2_enterprise,
    wpa3_psk,
    wpa2_wpa3_psk,
    wapi_psk,
};

/** @brief Maximum number of scan results retained by the shared Wi-Fi service. */
inline constexpr std::size_t kMaximumWifiScanResults = 32;
/** @brief IEEE 802.11 maximum SSID length used by all native adapters. */
inline constexpr std::size_t kMaximumWifiSsidLength = 32;

/** @brief Bounded access-point details used by Tasmota-compatible WiFiScan. */
struct WifiScanRecord final {
    std::array<char, kMaximumWifiSsidLength> ssid{};
    std::uint8_t ssid_length{};
    std::array<std::uint8_t, 6> bssid{};
    std::uint8_t channel{};
    std::int8_t rssi{};
    WifiEncryption encryption{WifiEncryption::unknown};
};

/** @brief Snapshot of both Wi-Fi interfaces; AP is not inferred from STA. */
struct WifiSnapshot final {
    WifiMode mode{WifiMode::off};
    WifiState station{WifiState::disabled};
    AccessPointState access_point{AccessPointState::disabled};
};

/** @brief Tasmota's time limit for a new network to connect: WifiTest uses it unless a
 *  platform's join needs longer, and a platform may give web setup more. */
inline constexpr std::uint32_t kWifiCredentialTestTimeoutMilliseconds = 9'000;

/** @brief Native Wi-Fi combinations that the adapter can keep alive together. */
struct WifiCapabilities final {
    bool station_access_point{};
    std::uint32_t credential_test_timeout_ms{kWifiCredentialTestTimeoutMilliseconds};
    std::uint32_t command_test_timeout_ms{kWifiCredentialTestTimeoutMilliseconds};
};

/** @brief Station link details for Tasmota's STATE Wifi block. */
struct WifiLink final {
    std::uint8_t bssid[6]{};
    std::uint8_t channel{}; ///< 0 when the backend cannot tell.
    const char* mode{};     ///< Negotiated PHY mode as Tasmota names it; nullptr when unknown.
};

/** @brief Names a station channel's band: 1..14 are 2.4 GHz, higher ones 5 GHz; nullptr for 0. */
[[nodiscard]] constexpr const char* wifi_band(const std::uint8_t channel) {
    return channel == 0U ? nullptr : channel <= 14U ? "2.4GHz" : "5GHz";
}

/** @brief Tasmota's chip limits for WifiPower, in tenths of a dBm. */
struct WifiTxPowerProfile final {
    std::uint8_t max_11b{};
    std::uint8_t max_54g{};
    std::uint8_t max_n{};
    std::int16_t sensitivity_11b{};
    std::int16_t sensitivity_54g{};
    std::int16_t sensitivity_n{};
};

/** @brief Bounded station identity and address fields used by StatusNET. */
struct NetworkInfo {
    char hostname[64]{};
    char address[16]{};
    char gateway[16]{};
    char subnet[16]{};
    char dns1[48]{};
    char dns2[48]{};
    char mac[18]{};
};

/** @brief Formats four network-order IPv4 bytes into a bounded buffer. */
template <std::size_t N> inline void format_ipv4(char (&output)[N], const void* address) {
    static_assert(N >= 16, "IPv4 text buffer is too small");
    const auto* bytes = static_cast<const std::uint8_t*>(address);
    std::snprintf(output, N, "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
}

/** @brief Formats a six-byte hardware address in Tasmota's uppercase notation. */
inline void format_mac(char (&output)[18], const void* address) {
    const auto* bytes = static_cast<const std::uint8_t*>(address);
    std::snprintf(output, sizeof(output), "%02X:%02X:%02X:%02X:%02X:%02X", bytes[0], bytes[1],
        bytes[2], bytes[3], bytes[4], bytes[5]);
}

/** @brief Returns the lower 24 bits used by Newmota's default identity. */
inline constexpr std::uint32_t device_id_from_mac(const std::uint8_t* const mac) {
    return (static_cast<std::uint32_t>(mac[3]) << 16U) |
           (static_cast<std::uint32_t>(mac[4]) << 8U) | static_cast<std::uint32_t>(mac[5]);
}

/** @brief Returns available native station information; unknown fields are empty. */
[[nodiscard]] NetworkInfo network_info();
/**
 * @brief Returns the current station's signal strength in dBm.
 * @return RSSI when connected and available; otherwise `std::nullopt`.
 */
[[nodiscard]] std::optional<std::int8_t> station_rssi();
/** @return true with the connected station's link details, false while not connected. */
[[nodiscard]] bool wifi_link(WifiLink& link);
/**
 * @brief Reports whether the station was refused for its credentials since the last connect.
 *
 * Defined only where Matter runs, which is its only caller.
 */
[[nodiscard]] bool wifi_auth_failed();
/** @brief Returns the stable lower-24-bit station MAC identity. */
[[nodiscard]] std::uint32_t device_id();
/** @brief Initializes the native Wi-Fi stack and its station/AP capability. */
[[nodiscard]] NetworkResult initialize_wifi();
/**
 * @brief Selects modem sleep (true) or no Wi-Fi power saving (false), as Tasmota's Sleep does.
 * @return unavailable where the backend keeps a fixed mode, success once applied.
 */
[[nodiscard]] NetworkResult set_wifi_power_save(bool enabled);
/** @brief Returns the chip's WifiPower limits; only adapters with USE_WIFI_POWER provide it. */
[[nodiscard]] WifiTxPowerProfile wifi_tx_power_profile();
/**
 * @brief Sets the maximum transmit power; only adapters with USE_WIFI_POWER provide it.
 * @param tenths_dbm Requested power; the adapter applies its nearest supported level.
 */
[[nodiscard]] NetworkResult set_wifi_tx_power(std::uint8_t tenths_dbm);
/** @brief Applies the resolved hostname to the native station/AP interfaces. */
[[nodiscard]] NetworkResult set_hostname(std::string_view hostname);
/** @brief Reports whether STA and SoftAP can remain active during a test. */
[[nodiscard]] WifiCapabilities wifi_capabilities();
/** @brief Which access point a station connection attempt joins. */
enum class WifiJoin : std::uint8_t {
    strongest,   ///< Scan every channel and join the strongest matching access point.
    first_found, ///< Join the first matching access point the scan finds, as Tasmota does.
};

/** @brief Starts one asynchronous station connection attempt. */
[[nodiscard]] NetworkResult connect_wifi(
    std::string_view ssid, std::string_view password, WifiJoin join = WifiJoin::strongest);
/** @brief Starts one asynchronous scan of visible access points. */
[[nodiscard]] NetworkResult wifi_scan_start();
/**
 * @brief Copies completed results from one scan without blocking.
 * @return busy while native scanning continues, success when complete.
 */
[[nodiscard]] NetworkResult wifi_scan_poll(
    WifiScanRecord* results, std::size_t capacity, std::size_t& count);
/**
 * @brief Requests cancellation when supported; callers keep polling until the
 * native scan has completed or returned an error.
 */
[[nodiscard]] NetworkResult wifi_scan_cancel();
/** @brief Returns the current native station and DHCP state. */
[[nodiscard]] WifiState wifi_state();
/** @brief Returns the latest station/AP lifecycle snapshot. */
[[nodiscard]] WifiSnapshot wifi_snapshot();
/** @brief Returns the AP-side address and identity, if the AP is active. */
[[nodiscard]] NetworkInfo access_point_info();
/**
 * @brief Starts the SoftAP and its platform DHCP service.
 *
 * The adapter copies both arguments before returning. An empty password means
 * an open AP, matching Tasmota's default AP passphrase.
 */
[[nodiscard]] NetworkResult start_access_point(std::string_view ssid, std::string_view password);
/** @brief Stops the SoftAP while leaving the Wi-Fi stack initialized. */
[[nodiscard]] NetworkResult stop_access_point();
/**
 * @brief Starts one asynchronous exchange, copying a request of at most 48 bytes.
 * @return success if accepted, busy while an earlier operation drains.
 * Start, poll and cancel are serialized by the single application context.
 */
[[nodiscard]] NetworkResult udp_start(std::string_view host, std::uint16_t port,
    const std::uint8_t* request, std::size_t request_size);
/** @brief Polls without waiting; success copies a response from the chosen peer.
 * @param peer_ipv4 Optional network-order IPv4 source bytes; zero for an IPv6 peer.
 */
[[nodiscard]] NetworkResult udp_poll(std::uint8_t* response, std::size_t response_capacity,
    std::size_t& response_size, std::array<std::uint8_t, 4>* peer_ipv4 = nullptr);
/** @brief Cancels without waiting; late DNS callbacks retain no caller memory. */
void udp_cancel();

} // namespace newmota::platform
