/*
  wifi_commands.cpp - Tasmota-compatible Wi-Fi commands for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/network/network_commands.h"

#include "newmota/platform/network.h"
#include "newmota/platform/platform.h"
#include "services/network/network_service.h"
#include "support/text.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace newmota::commands {
namespace {

/** @brief Small bounded builder for one MQTT-sized JSON command response. */
template <std::size_t N> class JsonBuilder final {
  public:
    [[nodiscard]] bool append(const std::string_view text) {
        if (text.size() > bytes_.size() - size_)
            return false;
        std::memcpy(bytes_.data() + size_, text.data(), text.size());
        size_ += text.size();
        return true;
    }

    [[nodiscard]] bool format(const char* const pattern, const int value) {
        std::array<char, 32> formatted{};
        const int length = std::snprintf(formatted.data(), formatted.size(), pattern, value);
        return length >= 0 && static_cast<std::size_t>(length) < formatted.size() &&
               append({formatted.data(), static_cast<std::size_t>(length)});
    }

    [[nodiscard]] bool json_string(const std::string_view text) {
        if (!append("\""))
            return false;
        for (std::size_t index = 0; index < text.size();) {
            const auto byte = static_cast<unsigned char>(text[index]);
            if (byte == '"' || byte == '\\') {
                const char escaped[2] = {'\\', static_cast<char>(byte)};
                if (!append({escaped, sizeof(escaped)}))
                    return false;
                ++index;
            } else if (byte < 0x20U) {
                constexpr char hex[] = "0123456789ABCDEF";
                const char escaped[] = {'\\', 'u', '0', '0', hex[byte >> 4U], hex[byte & 0x0FU]};
                if (!append({escaped, sizeof(escaped)}))
                    return false;
                ++index;
            } else if (byte < 0x80U) {
                if (!append({text.data() + index, 1U}))
                    return false;
                ++index;
            } else {
                const auto sequence = valid_utf8_sequence(text, index);
                if (sequence == 0U) {
                    constexpr char hex[] = "0123456789ABCDEF";
                    const char escaped[] = {
                        '\\', 'u', '0', '0', hex[byte >> 4U], hex[byte & 0x0FU]};
                    if (!append({escaped, sizeof(escaped)}))
                        return false;
                    ++index;
                } else {
                    if (!append(text.substr(index, sequence)))
                        return false;
                    index += sequence;
                }
            }
        }
        return append("\"");
    }

    [[nodiscard]] std::string_view view() const { return {bytes_.data(), size_}; }

    [[nodiscard]] std::size_t remaining() const { return bytes_.size() - size_; }

  private:
    [[nodiscard]] static std::size_t valid_utf8_sequence(
        const std::string_view text, const std::size_t index) {
        const auto first = static_cast<unsigned char>(text[index]);
        const std::size_t length = first >= 0xC2U && first <= 0xDFU   ? 2U
                                   : first >= 0xE0U && first <= 0xEFU ? 3U
                                   : first >= 0xF0U && first <= 0xF4U ? 4U
                                                                      : 0U;
        if (length == 0U || length > text.size() - index)
            return 0U;
        const auto second = static_cast<unsigned char>(text[index + 1U]);
        if ((second & 0xC0U) != 0x80U || (first == 0xE0U && second < 0xA0U) ||
            (first == 0xEDU && second >= 0xA0U) || (first == 0xF0U && second < 0x90U) ||
            (first == 0xF4U && second >= 0x90U)) {
            return 0U;
        }
        for (std::size_t offset = 2U; offset < length; ++offset) {
            if ((static_cast<unsigned char>(text[index + offset]) & 0xC0U) != 0x80U) {
                return 0U;
            }
        }
        return length;
    }

    std::array<char, N> bytes_{};
    std::size_t size_{};
};

/** @brief Returns the Tasmota encryption label for one portable auth mode. */
[[nodiscard]] std::string_view encryption_name(const platform::WifiEncryption encryption) {
    switch (encryption) {
    case platform::WifiEncryption::open:
        return "OPEN";
    case platform::WifiEncryption::wep:
        return "WEP";
    case platform::WifiEncryption::wpa_psk:
        return "WPA/PSK";
    case platform::WifiEncryption::wpa2_psk:
        return "WPA2/PSK";
    case platform::WifiEncryption::wpa_wpa2_psk:
        return "WPA/WPA2/PSK";
    case platform::WifiEncryption::wpa2_enterprise:
        return "WPA2/Enterprise";
    case platform::WifiEncryption::wpa3_psk:
        return "WPA3/PSK";
    case platform::WifiEncryption::wpa2_wpa3_psk:
        return "WPA2/WPA3/PSK";
    case platform::WifiEncryption::wapi_psk:
        return "WAPI/PSK";
    case platform::WifiEncryption::unknown:
        return "UNKNOWN";
    }
    return "UNKNOWN";
}

/** @brief Builds a bounded WiFiScan reply, keeping strongest results first. */
[[nodiscard]] bool write_wifi_scan_result(const NetworkService& network, CommandOutput& output) {
    JsonBuilder<1024> response;
    std::size_t result_count = 0U;
    const auto* const results = network.wifi_scan_results(result_count);
    if (result_count == 0U) {
        if (!response.append("{\"WiFiScan\":\"No networks found\"}")) {
            return false;
        }
    } else {
        if (!response.append("{\"WiFiScan\":{"))
            return false;
        std::size_t emitted = 0U;
        for (std::size_t index = 0; index < result_count; ++index) {
            const auto& result = results[index];
            JsonBuilder<384> item;
            if ((emitted != 0U && !item.append(",")) || !item.append("\"NET") ||
                !item.format("%d", static_cast<int>(index + 1U)) || !item.append("\":{\"SSId\":") ||
                !item.json_string({result.ssid.data(), result.ssid_length}) ||
                !item.append(",\"BSSId\":\"")) {
                return false;
            }
            char bssid[18]{};
            platform::format_mac(bssid, result.bssid.data());
            if (!item.append(bssid) || !item.append("\",\"Channel\":\"") ||
                !item.format("%d", static_cast<int>(result.channel)) ||
                !item.append("\",\"Signal\":\"") ||
                !item.format("%d", static_cast<int>(result.rssi)) ||
                !item.append("\",\"RSSI\":\"") ||
                !item.format("%d", wifi_rssi_quality(result.rssi)) ||
                !item.append("\",\"Encryption\":") ||
                !item.json_string(encryption_name(result.encryption)) || !item.append("}")) {
                return false;
            }
            if (response.remaining() < 2U || item.view().size() > response.remaining() - 2U) {
                break;
            }
            if (!response.append(item.view()))
                break;
            ++emitted;
        }
        if (!response.append("}}"))
            return false;
    }
    return output.write(response.view());
}

/** @brief Maps the retained candidate-test state to Tasmota status text. */
[[nodiscard]] std::string_view provisioning_status(
    const ProvisioningState state, const ProvisioningFailure failure) {
    switch (state) {
    case ProvisioningState::idle:
        return "Not Started";
    case ProvisioningState::testing:
        return "Testing";
    case ProvisioningState::succeeded:
        return "Successful";
    case ProvisioningState::failed:
        switch (failure) {
        case ProvisioningFailure::no_ip_address:
            return "Connect failed as no IP address received";
        case ProvisioningFailure::ap_unavailable:
            return "Connect failed";
        case ProvisioningFailure::ap_timeout:
            return "Connect failed with AP timeout";
        case ProvisioningFailure::none:
        case ProvisioningFailure::connect_failed:
            return "Connect failed";
        }
    }
    return "Not Started";
}

/** @brief Writes a canonical WifiTest response with its optional index. */
[[nodiscard]] bool write_wifi_test_status(
    const CommandRequest& request, CommandOutput& output, const std::string_view value) {
    JsonBuilder<96> response;
    if (!response.append("{\"WifiTest") ||
        (request.has_index && !response.format("%d", static_cast<int>(request.index))) ||
        !response.append("\":") || !response.json_string(value) || !response.append("}")) {
        return false;
    }
    return output.write(response.view());
}

} // namespace

/** @brief Serializes one scan result using Tasmota's asynchronous NETn shape. */
bool serialize_wifi_scan_announcement(const std::size_t network_number,
    const platform::WifiScanRecord* const result, char* const output, const std::size_t capacity,
    std::size_t& size) {
    size = 0U;
    if (output == nullptr || capacity == 0U)
        return false;
    JsonBuilder<384> response;
    if (result == nullptr) {
        if (!response.append("{\"WiFiScan\":\"No networks found\"}")) {
            return false;
        }
    } else {
        if (network_number == 0U || result->ssid_length > result->ssid.size() ||
            !response.append("{\"WiFiScan\":{\"NET") ||
            !response.format("%d", static_cast<int>(network_number)) ||
            !response.append("\":{\"SSId\":") ||
            !response.json_string({result->ssid.data(), result->ssid_length}) ||
            !response.append(",\"BSSId\":\"")) {
            return false;
        }
        char bssid[18]{};
        platform::format_mac(bssid, result->bssid.data());
        if (!response.append(bssid) || !response.append("\",\"Channel\":\"") ||
            !response.format("%d", static_cast<int>(result->channel)) ||
            !response.append("\",\"Signal\":\"") ||
            !response.format("%d", static_cast<int>(result->rssi)) ||
            !response.append("\",\"RSSI\":\"") ||
            !response.format("%d", wifi_rssi_quality(result->rssi)) ||
            !response.append("\",\"Encryption\":") ||
            !response.json_string(encryption_name(result->encryption)) || !response.append("}}}")) {
            return false;
        }
    }
    const auto payload = response.view();
    if (payload.size() > capacity)
        return false;
    std::memcpy(output, payload.data(), payload.size());
    size = payload.size();
    return true;
}

/** @brief Dispatches Tasmota WiFiScan and indexed WifiTest commands. */
CommandStatus dispatch_wifi_commands(
    const CommandRequest& request, NetworkService& network, CommandOutput& output) {
    const bool scan = support::ascii_iequals(request.name, "wifiscan");
    const bool test = support::ascii_iequals(request.name, "wifitest");
    if (!scan && !test)
        return CommandStatus::unknown;

    if (scan) {
        if (request.has_index)
            return CommandStatus::invalid;
        if (request.is_query()) {
            switch (network.wifi_scan_state()) {
            case WifiScanState::scanning:
                return output.write("{\"WiFiScan\":\"Scanning\"}") ? CommandStatus::handled
                                                                   : CommandStatus::failed;
            case WifiScanState::complete:
                return write_wifi_scan_result(network, output) ? CommandStatus::handled
                                                               : CommandStatus::failed;
            case WifiScanState::idle:
                return output.write("{\"WiFiScan\":\"Not Started\"}") ? CommandStatus::handled
                                                                      : CommandStatus::failed;
            case WifiScanState::failed:
                return output.write("{\"WiFiScan\":\"Failed\"}") ? CommandStatus::handled
                                                                 : CommandStatus::failed;
            }
            return CommandStatus::failed;
        }
        const auto result = network.start_wifi_scan(platform::monotonic_milliseconds());
        const auto value = result == platform::NetworkResult::success ? "Scanning"
                           : result == platform::NetworkResult::busy  ? "Busy"
                                                                      : "Not Started";
        return output.format("{\"WiFiScan\":\"%s\"}", value) ? CommandStatus::handled
                                                             : CommandStatus::failed;
    }

    if (request.has_index && request.index > 3U) {
        return CommandStatus::invalid;
    }
    if (request.is_query()) {
        return write_wifi_test_status(request, output,
                   provisioning_status(network.wifi_test_state(), network.wifi_test_failure()))
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    }
    const auto separator = request.payload.find('+');
    if (separator == std::string_view::npos || separator == 0U) {
        return CommandStatus::invalid;
    }
    const auto ssid = request.payload.substr(0U, separator);
    const auto password = request.payload.substr(separator + 1U);
    if (ssid.size() > 32U || password.size() > 64U || ssid.find('\0') != std::string_view::npos ||
        password.find('\0') != std::string_view::npos) {
        return CommandStatus::invalid;
    }
    const auto index = request.has_index ? request.index : static_cast<std::uint16_t>(1U);
    const auto result =
        network.start_wifi_test(ssid, password, index, platform::monotonic_milliseconds());
    const auto value = result == platform::NetworkResult::success       ? "Testing"
                       : result == platform::NetworkResult::busy        ? "Busy"
                       : result == platform::NetworkResult::unavailable ? "Connect failed"
                                                                        : "Connect failed";
    return write_wifi_test_status(request, output, value) ? CommandStatus::handled
                                                          : CommandStatus::failed;
}

} // namespace newmota::commands
