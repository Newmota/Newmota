/*
  network_identity.h - Tasmota-compatible Newmota identity formatting

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "newmota/platform/network.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace newmota::network_identity {

inline constexpr std::string_view kDefaultMqttTopic = "Newmota_%06X";
inline constexpr std::string_view kDefaultHostname = "%s-%04d";
/**
 * @brief Maximum runtime hostname because it is also the SoftAP SSID.
 *
 * Tasmota formats into a 32-byte hostname buffer with one byte reserved for
 * its terminator. Keep the same 31-byte payload bound on every adapter; this
 * also fits the Beken SDK's fixed 32-byte DHCP hostname storage.
 */
inline constexpr std::size_t kMaximumSoftApSsidLength = platform::kMaximumWifiSsidLength - 1U;
inline constexpr std::size_t kIdentityCapacity = 64;

/** @brief Owns the bounded runtime topic and hostname derived at boot. */
struct Resolved final {
    std::array<char, kIdentityCapacity> mqtt_topic{};
    std::array<char, kIdentityCapacity> hostname{};
    std::size_t mqtt_topic_size{};
    std::size_t hostname_size{};

    [[nodiscard]] std::string_view topic() const { return {mqtt_topic.data(), mqtt_topic_size}; }

    [[nodiscard]] std::string_view host() const { return {hostname.data(), hostname_size}; }
};

namespace detail {

template <std::size_t N>
[[nodiscard]] bool append_char(std::array<char, N>& output, std::size_t& size, const char character,
    const std::size_t maximum_size = N - 1U) {
    if (size >= maximum_size || size + 1U >= output.size())
        return false;
    output[size++] = character;
    output[size] = '\0';
    return true;
}

template <std::size_t N>
[[nodiscard]] bool append_hex(std::array<char, N>& output, std::size_t& size,
    const std::uint32_t value, const std::size_t digits) {
    constexpr char kDigits[] = "0123456789ABCDEF";
    for (std::size_t index = digits; index != 0U; --index) {
        const auto shift = static_cast<unsigned>((index - 1U) * 4U);
        if (!append_char(output, size, kDigits[(value >> shift) & 0x0FU])) {
            return false;
        }
    }
    return true;
}

template <std::size_t N>
[[nodiscard]] bool append_decimal(std::array<char, N>& output, std::size_t& size,
    std::uint32_t value, const std::size_t width, const std::size_t maximum_size = N - 1U) {
    std::array<char, 10> digits{};
    std::size_t count = 0U;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U && count < digits.size());
    const auto digit_count = count;
    while (count < width) {
        if (!append_char(output, size, '0', maximum_size))
            return false;
        ++count;
    }
    while (count > width) {
        --count;
    }
    for (std::size_t index = digit_count; index != 0U; --index) {
        if (!append_char(output, size, digits[index - 1U], maximum_size)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool hostname_character(const char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '.' || character == '-';
}

template <std::size_t N>
[[nodiscard]] bool append_hostname_char(std::array<char, N>& output, std::size_t& size,
    const char character, const std::size_t maximum_size = N - 1U) {
    return append_char(output, size, hostname_character(character) ? character : '-', maximum_size);
}

template <std::size_t N>
[[nodiscard]] bool append_hostname_text(std::array<char, N>& output, std::size_t& size,
    const std::string_view value, const std::size_t maximum_size) {
    for (const char character : value) {
        if (!append_hostname_char(output, size, character, maximum_size)) {
            return false;
        }
    }
    return true;
}

template <std::size_t N>
[[nodiscard]] bool finish_hostname(std::array<char, N>& output, const std::size_t size) {
    if (size == 0U)
        return false;
    if (output[0] == '-')
        output[0] = 'x';
    return true;
}

} // namespace detail

/** @brief Expands one topic template, retaining user-defined literal topics. */
inline bool resolve_topic(const std::string_view template_value, const std::uint32_t device_id,
    std::array<char, kIdentityCapacity>& output, std::size_t& size) {
    const auto source = template_value.empty() ? kDefaultMqttTopic : template_value;
    size = 0U;
    output = {};
    for (std::size_t index = 0U; index < source.size();) {
        if (source[index] == '%' && source.substr(index, 4U) == "%06X") {
            if (!detail::append_hex(output, size, device_id & 0x00FFFFFFU, 6U)) {
                return false;
            }
            index += 4U;
            continue;
        }
        if (!detail::append_char(output, size, source[index++]))
            return false;
    }
    return size != 0U;
}

/** @brief Expands and sanitizes the Tasmota-style runtime hostname. */
inline bool resolve_hostname(const std::string_view template_value, const std::string_view topic,
    const std::uint32_t device_id, std::array<char, kIdentityCapacity>& output, std::size_t& size) {
    const auto source = template_value.empty() ? kDefaultHostname : template_value;
    size = 0U;
    output = {};
    for (std::size_t index = 0U; index < source.size();) {
        if (source[index] == '%' && source.substr(index, 2U) == "%s") {
            if (!detail::append_hostname_text(output, size, topic, kMaximumSoftApSsidLength)) {
                return detail::finish_hostname(output, size);
            }
            index += 2U;
            continue;
        }
        if (source[index] == '%' && source.substr(index, 4U) == "%04d") {
            std::array<char, 16> decimal{};
            std::size_t decimal_size = 0U;
            if (!detail::append_decimal(decimal, decimal_size, device_id & 0x1FFFU, 4U)) {
                return false;
            }
            if (!detail::append_hostname_text(
                    output, size, {decimal.data(), decimal_size}, kMaximumSoftApSsidLength)) {
                return detail::finish_hostname(output, size);
            }
            index += 4U;
            continue;
        }
        if (source[index] == '%' && source.substr(index, 2U) == "%d") {
            std::array<char, 16> decimal{};
            std::size_t decimal_size = 0U;
            if (!detail::append_decimal(decimal, decimal_size, device_id & 0x1FFFU, 0U)) {
                return false;
            }
            if (!detail::append_hostname_text(
                    output, size, {decimal.data(), decimal_size}, kMaximumSoftApSsidLength)) {
                return detail::finish_hostname(output, size);
            }
            index += 2U;
            continue;
        }
        if (source[index] == '%' && source.substr(index, 4U) == "%06X") {
            std::array<char, 16> hexadecimal{};
            std::size_t hexadecimal_size = 0U;
            if (!detail::append_hex(hexadecimal, hexadecimal_size, device_id & 0x00FFFFFFU, 6U)) {
                return false;
            }
            if (!detail::append_hostname_text(output, size, {hexadecimal.data(), hexadecimal_size},
                    kMaximumSoftApSsidLength)) {
                return detail::finish_hostname(output, size);
            }
            index += 4U;
            continue;
        }
        if (!detail::append_hostname_char(
                output, size, source[index++], kMaximumSoftApSsidLength)) {
            return detail::finish_hostname(output, size);
        }
    }
    return detail::finish_hostname(output, size);
}

/** @brief Resolves the topic and hostname pair used by MQTT and SoftAP. */
inline bool resolve(const std::string_view topic_template, const std::string_view hostname_template,
    const std::uint32_t device_id, Resolved& output) {
    output = {};
    return resolve_topic(topic_template, device_id, output.mqtt_topic, output.mqtt_topic_size) &&
           resolve_hostname(
               hostname_template, output.topic(), device_id, output.hostname, output.hostname_size);
}

} // namespace newmota::network_identity
