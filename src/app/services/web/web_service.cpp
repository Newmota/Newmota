/*
  web_service.cpp - portable authenticated Compact web service for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file web_service.cpp
 * @brief Authenticated application API and embedded browser assets.
 */

#include "newmota_features.h"
#include "services/web/web_service.h"
#include "core/command/command.h"
#include "core/command/result_timestamp.h"
#include "core/message/message.h"
#include "core/storage/filesystem.h"
#include "core/system/boot_counter.h"
#include "core/system/boot_guard.h"
#if USE_PWM
#include "services/web/light_web.h"
#endif
#if USE_WEB_GPIO
#include "core/hardware/hardware_commands.h"
#endif
#if USE_TIMERS
#include "services/time/time_service.h"
#include "services/timers/timer_commands.h"
#endif
#if USE_SENSORS
#include "drivers/sensors/sensor_history.h"
#include "drivers/sensors/sensor_service.h"
#endif
#include "newmota/log.h"
#include "newmota/platform/network.h"
#include "newmota/platform/platform.h"
#include "services/network/network_service.h"
#include "services/settings/settings.h"
#if USE_WEB_HTTPS
#include "services/web/web_tls_identity.h"
#endif
#if USE_MQTT
#include "services/mqtt/mqtt_client.h"
#include "services/mqtt/mqtt_service.h"
#include "services/network/network_identity.h"
#endif
#if USE_WEB_AUDIO
#include "drivers/audio/i2s_service.h"
#include "services/web/audio_web.h"
#endif
#if USE_WEBCAM || USE_CSI_WEBCAM
#include "drivers/camera/camera_service.h"
#endif
#if USE_BERRY
#include "services/berry/berry_service.h"
#include "services/web/widgets.h"
#endif
#if USE_WEB_CONSOLE
#include "newmota/console_history.h"
#endif
#if USE_BEKEN_UPDATE_WEB
#include "services/web/update_web_service.h"
#endif
// Each platform build generates one page; Beken's has its own header.
#if __has_include("beken_web_assets.h")
#include "beken_web_assets.h"
#define NEWMOTA_BEKEN_WEB_PAGE 1
#else
#include "web_assets.h"
#endif
#include "support/text.h"
#include "support/number.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>

namespace newmota {
namespace {

constexpr std::size_t kWebCommandSourceCapacity = 512U;
#if USE_WEB_HTTPS
constexpr std::string_view kWebAssetEncoding{"br"};
constexpr char kWebAssetEncodingLabel[] = "Brotli";
constexpr char kWebAssetEncodingRequired[] = "Brotli required";
#else
constexpr std::string_view kWebAssetEncoding{"gzip"};
constexpr char kWebAssetEncodingLabel[] = "Gzip";
constexpr char kWebAssetEncodingRequired[] = "Gzip required";
#endif
#if USE_BERRY && USE_WEB_CONSOLE
constexpr std::uint32_t kBerryUploadIdleMilliseconds = 30000U;
constexpr std::size_t kBerryUploadIdLength = 32U;
constexpr std::size_t kBerryUploadEnvelopeSize = kBerryUploadIdLength + 1U;
constexpr std::size_t kBerryUploadChunkSize = 1800U;
#endif
#if defined(USE_BEKEN_UPDATE_WEB) && USE_BEKEN_UPDATE_WEB
constexpr std::size_t kOrdinaryRequestBodyCapacity = 1024U;
#else
constexpr std::size_t kOrdinaryRequestBodyCapacity = 512U;
#endif
// Console commands answer only while the Web Console is built in.
constexpr bool kWebConsole = USE_WEB_CONSOLE != 0;

/** @brief Compares secret bytes without an early exit on a mismatching character. */
bool equal_secret(const std::string_view left, const std::string_view right) {
    if (left.size() != right.size())
        return false;
    volatile unsigned int difference = 0;
    for (std::size_t index = 0; index < left.size(); ++index)
        difference |=
            static_cast<unsigned char>(left[index]) ^ static_cast<unsigned char>(right[index]);
    return difference == 0;
}

/** @brief Produces a 128-bit random token as fixed-length hexadecimal text. */
bool token(std::array<char, 33>& destination) {
    std::array<unsigned char, 16> random{};
    if (!platform::web_random(random.data(), random.size()))
        return false;
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t index = 0; index < random.size(); ++index) {
        destination[index * 2] = hex[random[index] >> 4];
        destination[index * 2 + 1] = hex[random[index] & 15];
    }
    destination[32] = '\0';
    return true;
}

#if USE_WEB_FILESYSTEM || (USE_BERRY && USE_WEB_CONSOLE)
/** @brief Parses an unsigned decimal size without accepting signs or overflow. */
bool parse_decimal_size(const std::string_view text, std::size_t& result) {
    if (text.empty())
        return false;
    std::size_t parsed = 0U;
    for (const char character : text) {
        if (character < '0' || character > '9')
            return false;
        const auto digit = static_cast<std::size_t>(character - '0');
        if (parsed > (std::numeric_limits<std::size_t>::max() - digit) / 10U)
            return false;
        parsed = parsed * 10U + digit;
    }
    result = parsed;
    return true;
}

/** @brief Parses Content-Range for a bounded sequential binary chunk. */
bool parse_content_range(
    const std::string_view text, std::size_t& offset, std::size_t& end, std::size_t& total) {
    constexpr std::string_view prefix{"bytes "};
    if (text.substr(0U, prefix.size()) != prefix)
        return false;
    const auto range = text.substr(prefix.size());
    const auto dash = range.find('-');
    const auto slash = range.find('/');
    if (dash == std::string_view::npos || slash == std::string_view::npos || dash == 0U ||
        slash <= dash + 1U || slash + 1U >= range.size() ||
        range.find('-', dash + 1U) != std::string_view::npos ||
        range.find('/', slash + 1U) != std::string_view::npos) {
        return false;
    }
    return parse_decimal_size(range.substr(0U, dash), offset) &&
           parse_decimal_size(range.substr(dash + 1U, slash - dash - 1U), end) &&
           parse_decimal_size(range.substr(slash + 1U), total) && end >= offset;
}

#endif
/** @brief Appends command output without allocation or partial successful replies. */
bool append(void* context, const std::string_view text) {
    auto& response = *static_cast<platform::WebResponse*>(context);
    if (text.size() > response.body.size() - response.size)
        return false;
    std::memcpy(response.body.data() + response.size, text.data(), text.size());
    response.size += text.size();
    return true;
}

/** @brief Leaves room for the submitting console's response correlation tag. */
bool append_console_command(void* context, const std::string_view text) {
    constexpr std::size_t kCorrelationTagReserve = 64U;
    auto& response = *static_cast<platform::WebResponse*>(context);
    const auto capacity = response.body.size() - kCorrelationTagReserve;
    if (response.size > capacity || text.size() > capacity - response.size) {
        return false;
    }
    std::memcpy(response.body.data() + response.size, text.data(), text.size());
    response.size += text.size();
    return true;
}

/** @brief Replaces a response with a short JSON error without reflecting user input. */
void error(platform::WebResponse& response, unsigned int status, const char* message) {
    response.status = status;
    response.size = 0;
    CommandOutput output{append, &response};
    (void)output.format("{\"error\":\"%s\"}", message);
}

/** @brief Accepts only the device address or assigned hostname, preventing DNS rebinding. */
bool local_host(std::string_view host, const bool secure) {
    const auto port = host.find(':');
    if (port != std::string_view::npos) {
        if (host.substr(port) != (secure ? ":443" : ":80"))
            return false;
        host = host.substr(0, port);
    }
    if (host.empty())
        return false;
    const auto network = platform::network_info();
    if (network.address[0] != '\0' && !support::ascii_iequals(network.address, "0.0.0.0") &&
        support::ascii_iequals(host, network.address))
        return true;
    const auto access_point = platform::access_point_info();
    if (access_point.address[0] != '\0' &&
        !support::ascii_iequals(access_point.address, "0.0.0.0") &&
        support::ascii_iequals(host, access_point.address))
        return true;
    if (network.hostname[0] == '\0')
        return false;
    if (support::ascii_iequals(host, network.hostname))
        return true;
    constexpr std::string_view suffix = ".local";
    return host.size() > suffix.size() &&
           support::ascii_iequals(host.substr(host.size() - suffix.size()), suffix) &&
           support::ascii_iequals(host.substr(0, host.size() - suffix.size()), network.hostname);
}

/** @brief Checks browser origin metadata independently of the session cookie. */
bool same_origin(const platform::WebRequest& request) {
    const std::string_view site{request.fetch_site.data()};
    if (!site.empty() && site != "same-origin" && site != "none")
        return false;
    const std::string_view origin{request.origin.data()};
    if (origin.empty())
        return true; // Non-browser clients still require credentials/CSRF.
    const std::string_view scheme = request.secure ? "https://" : "http://";
    return origin.substr(0, scheme.size()) == scheme &&
           support::ascii_iequals(origin.substr(scheme.size()), request.host.data());
}

/** @brief Parses an explicit compression offer, respecting a zero quality value. */
bool accepts_encoding(std::string_view encodings, const std::string_view wanted) {
    while (!encodings.empty()) {
        const auto end = encodings.find(',');
        auto item = support::trim_ascii(encodings.substr(0, end));
        const auto parameter = item.find(';');
        if (support::ascii_iequals(support::trim_ascii(item.substr(0, parameter)), wanted)) {
            if (parameter == std::string_view::npos)
                return true;
            item = support::trim_ascii(item.substr(parameter + 1));
            if (item.substr(0, 2) != "q=")
                return false;
            item.remove_prefix(2);
            return item == "1" || item == "1.0" || item == "1.00" || item == "1.000" ||
                   (item.size() >= 3 && item.size() <= 5 && item.substr(0, 2) == "0." &&
                       item.find_first_not_of("0123456789", 2) == std::string_view::npos &&
                       item.find_first_of("123456789", 2) != std::string_view::npos);
        }
        if (end == std::string_view::npos)
            break;
        encodings.remove_prefix(end + 1);
    }
    return false;
}

struct ProvisioningForm final {
    std::array<char, Settings::kSsidCapacity + 1U> ssid{};
    std::array<char, Settings::kPasswordCapacity + 1U> password{};
    std::array<char, Settings::kPasswordCapacity + 1U> web_password{};
    std::size_t ssid_size{};
    std::size_t password_size{};
    std::size_t web_password_size{};
    bool ssid_present{};
    bool password_present{};
    bool web_password_present{};
};

/** @brief Converts one URL-encoded hex digit without locale dependencies. */
int hex_digit(const char value) {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

/** @brief Decodes one bounded application/x-www-form-urlencoded field. */
template <std::size_t N>
bool decode_form_value(
    const std::string_view source, std::array<char, N>& target, std::size_t& size) {
    size = 0U;
    for (std::size_t index = 0; index < source.size(); ++index) {
        char value = source[index];
        if (value == '+') {
            value = ' ';
        } else if (value == '%') {
            if (index + 2U >= source.size())
                return false;
            const auto high = hex_digit(source[index + 1U]);
            const auto low = hex_digit(source[index + 2U]);
            if (high < 0 || low < 0)
                return false;
            value = static_cast<char>((high << 4) | low);
            index += 2U;
        }
        if (value == '\0' || value == '\r' || value == '\n' || size + 1U >= N) {
            return false;
        }
        target[size++] = value;
    }
    target[size] = '\0';
    return true;
}

/** @brief Parses the three explicit fields used by the onboarding form. */
bool parse_provisioning_form(const std::string_view body, ProvisioningForm& form) {
    form = {};
    auto remaining = body;
    while (!remaining.empty()) {
        const auto separator = remaining.find('&');
        const auto field = remaining.substr(0, separator);
        const auto equals = field.find('=');
        if (equals == std::string_view::npos)
            return false;
        const auto key = field.substr(0, equals);
        const auto value = field.substr(equals + 1U);
        if (key == "ssid") {
            if (form.ssid_present || !decode_form_value(value, form.ssid, form.ssid_size)) {
                return false;
            }
            form.ssid_present = true;
        } else if (key == "password") {
            if (form.password_present ||
                !decode_form_value(value, form.password, form.password_size)) {
                return false;
            }
            form.password_present = true;
        } else if (key == "webpassword") {
            if (form.web_password_present ||
                !decode_form_value(value, form.web_password, form.web_password_size)) {
                return false;
            }
            form.web_password_present = true;
        } else {
            return false;
        }
        if (separator == std::string_view::npos)
            break;
        remaining.remove_prefix(separator + 1U);
        if (remaining.empty())
            return false;
    }
    return form.ssid_present && form.password_present && form.web_password_present &&
           form.ssid_size != 0U;
}

#if USE_MQTT
/** @brief Parses a complete settings draft without executing console commands. */
bool parse_mqtt_form(std::string_view body, MqttSettings& candidate, std::uint16_t& tele_period) {
    constexpr std::array<std::string_view, 30> keys{"enabled", "host", "port", "client", "user",
        "topic", "fulltopic", "password_action", "password", "tele_period", "keep_alive", "retry",
        "timeout", "prefix1", "prefix2", "prefix3", "group1", "group2", "group3", "group4",
        "response_topic", "offline_lwt", "group_topic_mode", "only_json", "no_retain",
        "power_retain", "info_retain", "state_retain", "status_retain", "power_index"};
    std::uint32_t seen = 0U;
    std::array<char, 128> decoded{};
    std::array<char, MqttSettings::kPasswordCapacity> password{};
    std::size_t password_size = 0U;
    std::string_view action;
    std::array<char, 8> action_storage{};
    while (!body.empty()) {
        const auto amp = body.find('&');
        const auto field = body.substr(0U, amp);
        const auto equal = field.find('=');
        if (equal == std::string_view::npos)
            return false;
        const auto key = field.substr(0U, equal);
        std::size_t index = 0U;
        while (index < keys.size() && keys[index] != key)
            ++index;
        if (index == keys.size() || (seen & (1U << index)) != 0U)
            return false;
        seen |= 1U << index;
        std::size_t size = 0U;
        if (!decode_form_value(field.substr(equal + 1U), decoded, size))
            return false;
        const std::string_view value{decoded.data(), size};
        bool valid = true;
        std::uint32_t number = 0U;
        switch (index) {
        case 0:
            valid = value == "0" || value == "1";
            candidate.enabled = value == "1";
            break;
        case 1:
            valid = candidate.set_host(value);
            break;
        case 2:
            valid = support::parse_unsigned_decimal(value, UINT16_MAX, number) &&
                    candidate.set_port(static_cast<std::uint16_t>(number));
            break;
        case 3:
            valid = candidate.set_client_id(value);
            break;
        case 4:
            valid = candidate.set_username(value);
            break;
        case 5:
            valid = candidate.set_topic(value);
            break;
        case 6:
            valid = candidate.set_full_topic(value);
            break;
        case 7:
            valid = value == "preserve" || value == "replace" || value == "clear";
            if (valid) {
                std::memcpy(action_storage.data(), value.data(), size);
                action = {action_storage.data(), size};
            }
            break;
        case 8:
            valid = size < password.size();
            if (valid) {
                std::memcpy(password.data(), value.data(), size);
                password_size = size;
            }
            break;
        case 9:
            valid = support::parse_unsigned_decimal(value, UINT16_MAX, number);
            tele_period = static_cast<std::uint16_t>(number);
            break;
        case 10:
            valid = support::parse_unsigned_decimal(value, UINT16_MAX, number);
            candidate.keep_alive_seconds = static_cast<std::uint16_t>(number);
            break;
        case 11:
            valid = support::parse_unsigned_decimal(value, UINT16_MAX, number);
            candidate.retry_seconds = static_cast<std::uint16_t>(number);
            break;
        case 12:
            valid = support::parse_unsigned_decimal(value, UINT16_MAX, number);
            candidate.timeout_seconds = static_cast<std::uint16_t>(number);
            break;
        default:
            if (index <= 15U)
                valid = candidate.set_prefix(index - 13U, value);
            else if (index <= 19U)
                valid = candidate.set_group_topic(index - 16U, value);
            else {
                valid = value == "0" || value == "1";
                const bool flag = value == "1";
                switch (index) {
                case 20:
                    candidate.response_topic = flag;
                    break;
                case 21:
                    candidate.offline_lwt = flag;
                    break;
                case 22:
                    candidate.group_topic_mode = flag;
                    break;
                case 23:
                    candidate.only_json = flag;
                    break;
                case 24:
                    candidate.no_retain = flag;
                    break;
                case 25:
                    candidate.power_retain = flag;
                    break;
                case 26:
                    candidate.info_retain = flag;
                    break;
                case 27:
                    candidate.state_retain = flag;
                    break;
                case 28:
                    candidate.status_retain = flag;
                    break;
                case 29:
                    candidate.power_index = flag;
                    break;
                default:
                    valid = false;
                    break;
                }
            }
        }
        if (!valid)
            return false;
        if (amp == std::string_view::npos)
            break;
        body.remove_prefix(amp + 1U);
        if (body.empty())
            return false;
    }
    constexpr std::uint32_t required = 0xffU | (1U << 9U);
    if ((seen & required) != required)
        return false;
    if (action == "replace") {
        if ((seen & (1U << 8U)) == 0U || !candidate.set_password({password.data(), password_size}))
            return false;
    } else {
        if (password_size != 0U)
            return false;
        if (action == "clear" && !candidate.set_password({}))
            return false;
    }
    return candidate.valid() && (tele_period == 0U || (tele_period >= 10U && tele_period <= 3600U));
}

/** @brief Builds topic previews using the same bounded resolver as the runtime. */
bool write_mqtt_preview(
    CommandOutput& output, const MqttSettings& settings, const NetworkService* network) {
    const auto net = platform::network_info();
    std::array<char, 13> id{};
    std::size_t id_size = 0U;
    const auto device_id = platform::device_id();
    for (const char ch : std::string_view{net.mac}) {
        if (ch == ':')
            continue;
        const auto digit = hex_digit(ch);
        if (digit < 0 || id_size >= 12U)
            return false;
        constexpr char hex[] = "0123456789ABCDEF";
        id[id_size++] = hex[digit];
    }
    std::array<char, 64> resolved_topic{}, resolved_client{};
    std::size_t topic_size = 0U, client_size = 0U;
    if (!network_identity::resolve_topic(settings.topic(), device_id, resolved_topic, topic_size) ||
        !network_identity::resolve_topic(
            settings.client_id_template(), device_id, resolved_client, client_size))
        return false;
    MqttTopicPreview preview{};
    if (!MqttService::preview_topics(settings, {resolved_topic.data(), topic_size},
            {resolved_client.data(), client_size}, preview,
            network == nullptr ? std::string_view{net.hostname} : network->hostname(),
            {id.data(), id_size}))
        return false;
    return output.write("{\"command\":") && output.json_string(preview.command.data()) &&
           output.write(",\"result\":") && output.json_string(preview.result.data()) &&
           output.write(",\"state\":") && output.json_string(preview.state.data()) &&
           output.write(",\"lwt\":") && output.json_string(preview.lwt.data()) &&
           output.write(",\"fallback\":") && output.json_string(preview.fallback.data()) &&
           output.write(",\"groups\":[") && output.json_string(preview.groups[0].data()) &&
           output.write(",") && output.json_string(preview.groups[1].data()) && output.write(",") &&
           output.json_string(preview.groups[2].data()) && output.write(",") &&
           output.json_string(preview.groups[3].data()) && output.write("]}");
}

/** @brief Returns editable settings while keeping credentials write-only. */
bool write_mqtt_settings(CommandOutput& output, const Settings& owner) {
    const auto& mqtt = owner.mqtt_settings();
    bool okay =
        output.format(
            "{\"enabled\":%s,\"tls\":%s,\"tls_supported\":false,\"password_set\":%s,\"host\":",
            mqtt.enabled ? "true" : "false", mqtt.secure ? "true" : "false",
            mqtt.password().empty() ? "false" : "true") &&
        output.json_string(mqtt.host()) && output.write(",\"client\":") &&
        output.json_string(mqtt.client_id_template()) && output.write(",\"user\":") &&
        output.json_string(mqtt.username()) && output.write(",\"topic\":") &&
        output.json_string(mqtt.topic()) && output.write(",\"fulltopic\":") &&
        output.json_string(mqtt.full_topic()) &&
        output.format(
            ",\"port\":%u,\"keep_alive\":%u,\"retry\":%u,\"timeout\":%u,\"tele_period\":%u",
            static_cast<unsigned>(mqtt.port), static_cast<unsigned>(mqtt.keep_alive_seconds),
            static_cast<unsigned>(mqtt.retry_seconds), static_cast<unsigned>(mqtt.timeout_seconds),
            static_cast<unsigned>(owner.tele_period_seconds()));
    for (std::size_t i = 0U; okay && i < 3U; ++i) {
        okay = output.format(",\"prefix%u\":", static_cast<unsigned>(i + 1U)) &&
               output.json_string(mqtt.prefix(i));
    }
    for (std::size_t i = 0U; okay && i < 4U; ++i) {
        okay = output.format(",\"group%u\":", static_cast<unsigned>(i + 1U)) &&
               output.json_string(mqtt.group_topic(i));
    }
    constexpr std::array<std::string_view, 10> names{"response_topic", "offline_lwt",
        "group_topic_mode", "only_json", "no_retain", "power_retain", "info_retain", "state_retain",
        "status_retain", "power_index"};
    const std::array<bool, 10> flags{mqtt.response_topic, mqtt.offline_lwt, mqtt.group_topic_mode,
        mqtt.only_json, mqtt.no_retain, mqtt.power_retain, mqtt.info_retain, mqtt.state_retain,
        mqtt.status_retain, mqtt.power_index};
    for (std::size_t i = 0U; okay && i < names.size(); ++i) {
        okay = output.write(",") && output.json_string(names[i]) && output.write(":") &&
               output.write(flags[i] ? "true" : "false");
    }
    return okay && output.write("}");
}
#endif

/** @brief Accepts a media type with optional parameters. */
bool content_type_is(const std::string_view value, const std::string_view expected) {
    const auto separator = value.find(';');
    return support::ascii_iequals(support::trim_ascii(value.substr(0, separator)), expected);
}

/** @brief Hidden staging directory on the volume that receives @p target. */
std::string_view upload_staging_directory(const std::string_view target) {
    return filesystem::volume(target) == filesystem::Volume::sd ? "/sd/.newmota" : "/.newmota";
}

/** @brief Staging file on the volume that receives @p target, so the commit stays one rename. */
const char* upload_staging_file(const std::string_view target) {
    return filesystem::volume(target) == filesystem::Volume::sd ? "/sd/.newmota/web-upload.tmp"
                                                                : "/.newmota/web-upload.tmp";
}

#if USE_WEB_FILESYSTEM
constexpr std::size_t kFilesystemPathCapacity = 192U;
constexpr std::size_t kFilesystemPageEntries = 32U;
constexpr std::size_t kFilesystemFooterReserve = 48U;

/** @brief Accepts only canonical absolute paths within the shared FS bound. */
bool canonical_filesystem_path(const std::string_view path) {
    if (path.empty() || path.size() >= kFilesystemPathCapacity || path.front() != '/' ||
        path.find('\0') != std::string_view::npos || (path.size() > 1U && path.back() == '/')) {
        return false;
    }
    if (path == "/")
        return true;

    std::size_t component_start = 1U;
    for (std::size_t index = 1U; index <= path.size(); ++index) {
        if (index != path.size() && path[index] != '/') {
            const auto character = static_cast<unsigned char>(path[index]);
            if (character < 0x20U || character == 0x7fU)
                return false;
            continue;
        }
        const auto component = path.substr(component_start, index - component_start);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        component_start = index + 1U;
    }
    return true;
}

struct FilesystemListRequest final {
    std::uint32_t offset{};
    std::string_view path{};
};

/** @brief Parses the bounded offset/newline/path request body. */
bool parse_filesystem_list_request(const std::string_view body, FilesystemListRequest& request) {
    const auto separator = body.find('\n');
    if (separator == std::string_view::npos ||
        body.find('\n', separator + 1U) != std::string_view::npos) {
        return false;
    }
    if (!support::parse_unsigned_decimal(body.substr(0U, separator),
            std::numeric_limits<std::uint32_t>::max() -
                static_cast<std::uint32_t>(kFilesystemPageEntries),
            request.offset)) {
        return false;
    }
    request.path = body.substr(separator + 1U);
    return canonical_filesystem_path(request.path);
}

/** @brief Computes the bytes required for the shared JSON string encoding. */
std::size_t json_string_size(const std::string_view text) {
    std::size_t size = 2U;
    for (const char value : text) {
        const auto character = static_cast<unsigned char>(value);
        if (character == '"' || character == '\\' || character == '\b' || character == '\f' ||
            character == '\n' || character == '\r' || character == '\t') {
            size += 2U;
        } else if (character < 0x20U) {
            size += 6U;
        } else {
            ++size;
        }
    }
    return size;
}

/** @brief Counts decimal digits without formatting or allocating. */
std::size_t decimal_digits(std::uint32_t value) {
    std::size_t digits = 1U;
    while (value >= 10U) {
        value /= 10U;
        ++digits;
    }
    return digits;
}

struct FilesystemListOutput final {
    CommandOutput* output{};
    std::uint32_t offset{};
    std::uint32_t next{};
    std::size_t visited{};
    std::size_t emitted{};
    bool more{};
    bool too_large{};
};

/** @brief Serializes one direct child while keeping the response page bounded. */
bool write_filesystem_entry(void* const context, const filesystem::DirectoryEntry& entry) {
    auto& page = *static_cast<FilesystemListOutput*>(context);
    if (page.visited++ < page.offset)
        return true;
    if (page.emitted >= kFilesystemPageEntries) {
        page.more = true;
        return false;
    }

    const std::string_view type =
        entry.type == filesystem::EntryType::directory ? "directory" : "file";
    constexpr std::size_t kEntryFixedSize = 25U;
    const auto entry_size = kEntryFixedSize + json_string_size(entry.name) +
                            json_string_size(type) + decimal_digits(entry.size_bytes) +
                            (page.emitted == 0U ? 0U : 1U);
    const auto response_capacity = page.output->size() <= 4096U ? 4096U - page.output->size() : 0U;
    if (entry_size > response_capacity ||
        kFilesystemFooterReserve > response_capacity - entry_size) {
        if (page.emitted == 0U) {
            page.too_large = true;
        } else {
            page.more = true;
        }
        return false;
    }

    auto& output = *page.output;
    if ((page.emitted != 0U && !output.write(",")) || !output.write("{\"name\":") ||
        !output.json_string(entry.name) || !output.write(",\"type\":") ||
        !output.json_string(type) ||
        !output.format(",\"size\":%lu}", static_cast<unsigned long>(entry.size_bytes))) {
        return false;
    }
    ++page.emitted;
    page.next = page.offset + static_cast<std::uint32_t>(page.emitted);
    return true;
}

#endif

#if USE_WEB_CONSOLE
struct ConsoleCursors final {
    std::uint32_t web{};
    std::uint32_t berry{};
};

/** @brief Parses the two required sequence cursors from a bounded form body. */
bool parse_console_cursors(const std::string_view body, ConsoleCursors& cursors) {
    cursors = {};
    bool web_present = false;
    bool berry_present = false;
    auto remaining = body;
    while (!remaining.empty()) {
        const auto separator = remaining.find('&');
        const auto field = remaining.substr(0U, separator);
        const auto equals = field.find('=');
        if (equals == std::string_view::npos)
            return false;
        const auto key = field.substr(0U, equals);
        const auto value = field.substr(equals + 1U);
        std::uint32_t cursor = 0U;
        if (!support::parse_unsigned_decimal(
                value, std::numeric_limits<std::uint32_t>::max(), cursor)) {
            return false;
        }
        if (key == "web" && !web_present) {
            cursors.web = cursor;
            web_present = true;
        } else if (key == "berry" && !berry_present) {
            cursors.berry = cursor;
            berry_present = true;
        } else {
            return false;
        }
        if (separator == std::string_view::npos)
            break;
        remaining.remove_prefix(separator + 1U);
        if (remaining.empty())
            return false;
    }
    return web_present && berry_present;
}

struct ConsoleEventWriter final {
    CommandOutput* output{};
    bool first{true};
};

/** @brief Emits one bounded history item as a JSON object. */
bool write_console_event(void* const context, const std::uint32_t sequence,
    const console_history::OutputKind kind, const std::uint32_t group,
    const std::string_view text) {
    auto& writer = *static_cast<ConsoleEventWriter*>(context);
    if ((!writer.first && !writer.output->write(",")) || !writer.output->write("{\"sequence\":") ||
        !writer.output->format("%lu", static_cast<unsigned long>(sequence)) ||
        !writer.output->write(",\"type\":\"") ||
        !writer.output->write(
            kind == console_history::OutputKind::command_result ? "result" : "log") ||
        !writer.output->write("\",\"group\":") ||
        !writer.output->format("%lu,\"text\":", static_cast<unsigned long>(group)) ||
        !writer.output->json_string(text) || !writer.output->write("}")) {
        return false;
    }
    writer.first = false;
    return true;
}

/** @brief Writes one console history batch and its independent cursor state. */
bool write_console_batch(CommandOutput& output, const bool berry, const std::uint32_t cursor) {
    constexpr std::size_t kEventsPerPoll = 4U;
    if (!output.write("{\"items\":["))
        return false;
    ConsoleEventWriter writer{&output};
    const auto result = berry ? console_history::read_berry_output(
                                    cursor, kEventsPerPoll, write_console_event, &writer)
                              : console_history::read_web_output(
                                    cursor, kEventsPerPoll, write_console_event, &writer);
    return output.write("],\"next\":") &&
           output.format("%lu,\"dropped\":%s,\"reset\":%s,\"more\":%s,\"count\":%lu}",
               static_cast<unsigned long>(result.next), result.dropped ? "true" : "false",
               result.reset ? "true" : "false", result.more ? "true" : "false",
               static_cast<unsigned long>(result.count));
}

#endif

/** @brief Serializes the shared provisioning state for the browser poller. */
bool write_provisioning_state(CommandOutput& output, const NetworkService& network,
    const Settings& settings, const platform::WebRequest& request) {
    const auto info =
        network.access_point() ? platform::access_point_info() : platform::network_info();
    const char* state = "idle";
    switch (network.provisioning_state()) {
    case ProvisioningState::idle:
        state = "idle";
        break;
    case ProvisioningState::testing:
        state = "testing";
        break;
    case ProvisioningState::succeeded:
        state = "succeeded";
        break;
    case ProvisioningState::failed:
        state = "failed";
        break;
    }
    const char* mode = network.onboarding() ? "onboarding"
                       : network.recovery() ? "recovery"
                                            : "station";
    (void)output.write("{\"state\":");
    (void)output.json_string(state);
    (void)output.write(",\"mode\":");
    (void)output.json_string(mode);
    (void)output.write(",\"address\":");
    (void)output.json_string(info.address);
    (void)output.write(",\"auth_required\":");
    (void)output.write(
        network.onboarding() ? "false" : (settings.web_password().empty() ? "false" : "true"));
    (void)output.write(",\"restart_pending\":");
    (void)output.write(network.restart_scheduled() ? "true" : "false");
    (void)output.write(",\"secure\":");
    (void)output.write(request.secure ? "true" : "false");
    return output.write("}");
}

} // namespace

/** @brief Binds web behavior to commands and canonical message snapshots. */
WebService::WebService(Settings& settings, CommandProcessor& commands, MessageRouter& messages,
    NetworkService* network, UpdateWebService* updates, BerryService* berry,
    BootCounter* boot_counter)
    : settings_(settings), commands_(commands), messages_(messages), boot_counter_(boot_counter),
      network_(network), updates_(updates), berry_(berry) {}

/** @brief Drops any unfinished in-memory Berry source. */
WebService::~WebService() {
    clear_berry_upload();
    clear_filesystem_upload();
}

/** @brief Invalidates cookies after password replacement or a settings reset. */
void WebService::refresh_credential() {
    const auto password = settings_.web_password();
    if (equal_secret(password, {credential_.data(), credential_size_}))
        return;
    sessions_ = {};
    clear_berry_upload();
    clear_filesystem_upload();
    credential_.fill(0);
    credential_size_ = password.size();
    std::memcpy(credential_.data(), password.data(), password.size());
}

/** @brief Discards staged bytes without changing the destination. */
void WebService::clear_filesystem_upload() {
    if (filesystem_upload_.writer.native != nullptr) {
        (void)filesystem::close(filesystem_upload_.writer);
        (void)filesystem::remove_if_exists(upload_staging_file(filesystem_upload_.path.data()));
    }
    filesystem_upload_ = {};
}

void WebService::expire_filesystem_upload(std::uint32_t now) {
    if (filesystem_upload_active() && now - filesystem_upload_.last_activity >= 30000U)
        clear_filesystem_upload();
}

/** @brief Drops a partial source and all metadata without retaining heap storage. */
void WebService::clear_berry_upload() {
    delete[] berry_upload_.source;
    berry_upload_.source = nullptr;
    berry_upload_.id.fill(0);
    berry_upload_.owner_session.fill(0);
    berry_upload_.total = 0U;
    berry_upload_.received = 0U;
    berry_upload_.last_activity = 0U;
}

/** @brief Reclaims an abandoned upload after thirty seconds without a chunk. */
void WebService::expire_berry_upload(const std::uint32_t now) {
#if USE_BERRY && USE_WEB_CONSOLE
    if (berry_upload_.source != nullptr &&
        now - berry_upload_.last_activity >= kBerryUploadIdleMilliseconds) {
        clear_berry_upload();
    }
#else
    (void)now;
#endif
}

/** @brief Finds an exact cookie name and expires sessions after 30 minutes of inactivity. */
WebService::Session* WebService::session(std::string_view cookie, const std::uint32_t now) {
    std::string_view value;
    while (!cookie.empty()) {
        const auto end = cookie.find(';');
        const auto part = support::trim_ascii(cookie.substr(0, end));
        constexpr std::string_view prefix = "nm_session=";
        if (part.substr(0, prefix.size()) == prefix) {
            if (!value.empty())
                return nullptr;
            value = part.substr(prefix.size());
        }
        if (end == std::string_view::npos)
            break;
        cookie.remove_prefix(end + 1);
    }
    Session* found = nullptr;
    for (auto& entry : sessions_) {
        if (entry.token[0] != '\0' && now - entry.last_used >= 1800000U)
            entry = {};
        if (value.size() == 32 && equal_secret(value, entry.token.data()))
            found = &entry;
    }
    return found;
}

/** @brief Starts on either Wi-Fi interface, then processes one request. */
bool WebService::poll(const std::uint32_t now, const bool network_ready) {
    refresh_credential();
    expire_berry_upload(now);
    expire_filesystem_upload(now);
    if (!started_ && network_ready && (!attempted_start_ || now - last_start_ >= 5000U)) {
#if USE_WEB_HTTPS
        const bool identity_ready = prepare_web_tls_identity();
        started_ = identity_ready && platform::web_start(&web_tls_identity());
#else
        started_ = platform::web_start();
#endif
        attempted_start_ = true;
        last_start_ = now;
        if (started_) {
            auto network = platform::network_info();
            if (network.address[0] == '\0' || support::ascii_iequals(network.address, "0.0.0.0")) {
                network = platform::access_point_info();
            }
#if USE_WEB_HTTPS
            AddLog(LogLevel::info, "WEB: HTTPS listening on port 443 at %s",
#else
            AddLog(LogLevel::info, "WEB: HTTP listening on port 80 at %s",
#endif
                network.address[0] != '\0' && !support::ascii_iequals(network.address, "0.0.0.0")
                    ? network.address
                    : "unknown address");
        } else {
#if USE_WEB_HTTPS
            AddLog(LogLevel::error, "WEB: HTTPS identity or server start failed");
#else
            AddLog(LogLevel::error, "WEB: HTTP start failed");
#endif
        }
    }
    if (auto* exchange = platform::web_request()) {
        handle(*exchange, now);
        platform::web_complete();
        return true;
    }
    return false;
}

/** @brief Implements the small same-origin API; commands never execute on the SDK task. */
void WebService::handle(platform::WebExchange& exchange, const std::uint32_t now) {
    refresh_credential();
    expire_berry_upload(now);
    expire_filesystem_upload(now);
    const auto& request = exchange.request;
    auto& response = exchange.response;
    if (!local_host(request.host.data(), request.secure)) {
        error(response, 403, "Host rejected");
        return;
    }
    const std::string_view path{request.path.data()};
    const bool post = request.method == platform::WebMethod::post;
    if (!post && path == "/") {
        if (!accepts_encoding(request.accept_encoding.data(), kWebAssetEncoding)) {
            error(response, 406, kWebAssetEncodingRequired);
            return;
        }
        response.content_type = "text/html; charset=utf-8";
        response.content_encoding = kWebAssetEncoding.data();
#ifdef NEWMOTA_BEKEN_WEB_PAGE
        response.asset = reinterpret_cast<const char*>(generated::beken_web::kIndex);
        response.size = sizeof(generated::beken_web::kIndex);
#else
        response.asset = reinterpret_cast<const char*>(generated::web::kIndex);
        response.size = sizeof(generated::web::kIndex);
#endif
        return;
    }
    if (!same_origin(request)) {
        error(response, 403, "Origin rejected");
        return;
    }

    const bool berry_chunk_request = post && path == "/api/berry/console/chunk";
    const auto request_body_limit = path == "/api/berry/console" || berry_chunk_request ||
                                            path == "/api/filesystem/upload/chunk" ||
                                            path == "/api/mqtt" || path == "/api/mqtt/preview"
                                        ? static_cast<std::size_t>(NEWMOTA_WEB_REQUEST_BODY_SIZE)
                                        : kOrdinaryRequestBodyCapacity;
    if (post && request.body_size > request_body_limit) {
        error(response, 413, "Request body exceeds the endpoint limit");
        return;
    }

    const bool onboarding = network_ != nullptr && network_->onboarding();
    const bool recovery = network_ != nullptr && network_->recovery();
    const bool auth_required = credential_size_ != 0U && !onboarding;
    CommandOutput output{append, &response};

    if (!post && path == "/api/bootstrap") {
        const auto network = platform::network_info();
        const auto access_point = platform::access_point_info();
        const auto& local_network =
            network.address[0] != '\0' && !support::ascii_iequals(network.address, "0.0.0.0")
                ? network
                : access_point;
        const char* mode = onboarding ? "onboarding" : recovery ? "recovery" : "station";
        (void)output.write("{\"mode\":");
        (void)output.json_string(mode);
        (void)output.write(",\"auth_required\":");
        (void)output.write(auth_required ? "true" : "false");
        (void)output.write(",\"address\":");
        (void)output.json_string(local_network.address);
        (void)output.write(",\"ap_ssid\":");
        (void)output.json_string(
            network_ == nullptr ? std::string_view{} : network_->access_point_ssid());
        (void)output.write(",\"station_access_point\":");
        (void)output.write(
            network_ != nullptr && network_->provisioning_supported() ? "true" : "false");
        (void)output.write("}");
        return;
    }

    const bool provisioning_apply = post && path == "/api/onboarding/apply";
    const bool provisioning_status = !post && path == "/api/onboarding/status";
    if (provisioning_apply && onboarding) {
        if (!content_type_is(request.content_type.data(), "application/x-www-form-urlencoded")) {
            error(response, 415, "Use form encoding");
            return;
        }
        if (network_ == nullptr) {
            error(response, 503, "Wi-Fi service unavailable");
            return;
        }
        ProvisioningForm form{};
        if (!parse_provisioning_form({request.body.data(), request.body_size}, form)) {
            error(response, 400, "Invalid Wi-Fi form");
            return;
        }
        const auto result = network_->start_provisioning_test({form.ssid.data(), form.ssid_size},
            {form.password.data(), form.password_size},
            {form.web_password.data(), form.web_password_size}, now);
        if (result == platform::NetworkResult::success) {
            response.status = 202;
            (void)output.write("{\"state\":\"testing\"}");
        } else if (result == platform::NetworkResult::busy) {
            error(response, 409,
                network_->wifi_scan_pending() ? "Wi-Fi scan is still running"
                                              : "Wi-Fi test already running");
        } else if (result == platform::NetworkResult::unavailable) {
            error(response, 503, "Wi-Fi test unavailable");
        } else {
            error(response, 400, "Invalid Wi-Fi credentials");
        }
        return;
    }

    if (provisioning_status && onboarding) {
        if (network_ == nullptr) {
            error(response, 404, "Endpoint not found");
            return;
        }
        (void)write_provisioning_state(output, *network_, settings_, request);
        return;
    }

    const bool update_chunk_request = post && path == "/api/update/chunk";
#if USE_BERRY && USE_WEB_CONSOLE
    const bool berry_upload_begin_request = post && path == "/api/berry/console/begin";
    const bool berry_upload_control_request =
        post && (path == "/api/berry/console/run" || path == "/api/berry/console/abort");
#endif
    if (post && (provisioning_apply || path == "/api/mqtt" || path == "/api/mqtt/preview")) {
        if (!content_type_is(request.content_type.data(), "application/x-www-form-urlencoded")) {
            error(response, 415, "Use form encoding");
            return;
        }
    } else if (update_chunk_request || (post && path == "/api/filesystem/upload/chunk")) {
        if (!content_type_is(request.content_type.data(), "application/octet-stream")) {
            error(response, 415, "Use application/octet-stream");
            return;
        }
    } else if (berry_chunk_request) {
        if (!content_type_is(request.content_type.data(), "application/octet-stream")) {
            error(response, 415, "Use application/octet-stream");
            return;
        }
    } else if (post && !content_type_is(request.content_type.data(), "text/plain")) {
        error(response, 415, "Use text/plain");
        return;
    }

    auto* current = auth_required ? session(request.cookie.data(), now) : nullptr;
    if (post && path == "/api/login") {
        if (!auth_required) {
            error(response, 400, "Authentication disabled");
            return;
        }
        if (std::string_view{request.csrf.data()} != "login") {
            error(response, 403, "Login header required");
            return;
        }
        if (now - login_window_ >= 30000U) {
            login_window_ = now;
            login_attempts_ = 0;
        }
        if (login_attempts_ >= 5) {
            error(response, 429, "Try again later");
            return;
        }
        ++login_attempts_;
        if (!equal_secret(
                {request.body.data(), request.body_size}, {credential_.data(), credential_size_})) {
            error(response, 401, "Login failed");
            return;
        }
        if (current == nullptr) {
            for (auto& entry : sessions_) {
                if (entry.token[0] == '\0') {
                    current = &entry;
                    break;
                }
            }
        }
        if (current == nullptr) {
            error(response, 429, "Two sessions already active");
            return;
        }
        *current = {};
        if (!token(current->token) || !token(current->csrf)) {
            *current = {};
            error(response, 503, "Session entropy unavailable");
            return;
        }
        current->last_used = now;
        std::snprintf(response.cookie.data(), response.cookie.size(),
            "nm_session=%s; Path=/; HttpOnly; SameSite=Strict%s", current->token.data(),
            request.secure ? "; Secure" : "");
        CommandOutput output{append, &response};
        (void)output.format("{\"csrf\":\"%s\"}", current->csrf.data());
        return;
    }
    if (auth_required) {
        if (current == nullptr) {
            error(response, 401, "Login required");
            return;
        }
#if USE_BEKEN_UPDATE_WEB
        const bool self_paced = UpdateWebService::self_paced(post, path);
#else
        constexpr bool self_paced = false;
#endif
        if (!self_paced) {
            if (now - current->rate_start >= 1000U) {
                current->rate_start = now;
                current->requests = 0;
            }
            if (current->requests >= 25) {
                error(response, 429, "Request rate exceeded");
                return;
            }
            ++current->requests;
        }
        if (post && !equal_secret(request.csrf.data(), current->csrf.data())) {
            error(response, 403, "CSRF token required");
            return;
        }
        current->last_used = now;
    }
#if USE_TIMERS
    if (!post && path == "/api/timers") {
        if (!commands::write_timers_overview(
                output, settings_, time_ != nullptr && time_->synchronized()))
            error(response, 507, "Timer data exceeded response limit");
        return;
    }
#endif
#if USE_WEB_GPIO
    if (!post && (path == "/api/gpio" || path == "/api/gpio/catalog")) {
        if (!(path == "/api/gpio" ? commands::write_gpio_state(output, settings_)
                                  : commands::write_gpio_catalog(output)))
            error(response, 507, "GPIO data exceeded response limit");
        return;
    }
#endif
#if USE_PWM
    if (path == "/api/light") {
        if (!post) {
            if (!light_web::write_lights(output, light_))
                error(response, 507, "Light state exceeded response limit");
            return;
        }
        switch (light_web::apply(light_, {request.body.data(), request.body_size}, output)) {
        case light_web::Result::ok:
            break;
        case light_web::Result::invalid:
            error(response, 400, "Invalid light request");
            break;
        case light_web::Result::unavailable:
            error(response, 409, "Light unavailable");
            break;
        case light_web::Result::failed:
            error(response, 500, "Light update failed");
            break;
        }
        return;
    }
#endif
#if USE_BEKEN_UPDATE_WEB
    if ((!filesystem_access_allowed_ || filesystem_upload_active()) && post &&
        path == "/api/update/begin") {
        error(response, 409, "Filesystem transfer is active");
        return;
    }
    if (updates_ != nullptr && updates_->handle(request, response, now))
        return;
#endif
#if USE_WEB_FILESYSTEM
    const bool filesystem_request = post && path.substr(0, 16) == "/api/filesystem/";
    if (filesystem_request && (!filesystem_access_allowed_
#if USE_BEKEN_UPDATE_WEB
                                  || (updates_ != nullptr && updates_->active())
#endif
                                      )) {
        error(response, 409, "Firmware update is active");
        return;
    }
    const std::string_view filesystem_body{request.body.data(), request.body_size};
    const auto mutable_path = [](std::string_view target) {
        if (filesystem::volume(target) == filesystem::Volume::sd)
            target = target.substr(3); // The card root and its staging area are protected too.
        return canonical_filesystem_path(target) && !target.empty() && target != "/" &&
               target != "/.newmota" && target.substr(0, 10) != "/.newmota/";
    };
    const auto stat_error_status = [](filesystem::ReadResult result) -> unsigned {
        switch (result) {
        case filesystem::ReadResult::unavailable:
            return 503;
        case filesystem::ReadResult::invalid_path:
            return 400;
        case filesystem::ReadResult::not_found:
            return 404;
        default:
            return 500;
        }
    };
    if (post && path == "/api/filesystem/read") {
        FilesystemListRequest read_request{};
        if (!parse_filesystem_list_request(filesystem_body, read_request) ||
            !mutable_path(read_request.path)) {
            error(response, 400, "Invalid filesystem path or offset");
            return;
        }
        filesystem::EntryType type{};
        std::uint32_t size{};
        const auto stat = filesystem::stat(read_request.path, type, size);
        if (stat != filesystem::ReadResult::succeeded || type != filesystem::EntryType::file) {
            error(response,
                stat == filesystem::ReadResult::succeeded ? 404 : stat_error_status(stat),
                "File unavailable");
            return;
        }
        if (read_request.offset > size) {
            error(response, 400, "Invalid read offset");
            return;
        }
        filesystem::FileReader reader{};
        if (filesystem::open_reader(read_request.path, reader) !=
            filesystem::ReadResult::succeeded) {
            error(response, 500, "File read failed");
            return;
        }
        const bool okay =
            filesystem::seek(reader, read_request.offset) == filesystem::ReadResult::succeeded &&
            filesystem::read(reader, response.body.data(), response.body.size(), response.size) ==
                filesystem::ReadResult::succeeded;
        const bool closed = filesystem::close(reader);
        if (!okay || !closed) {
            error(response, 500, "File read failed");
            return;
        }
        response.content_type = "application/octet-stream";
        return;
    }
    if (post && path == "/api/filesystem/delete") {
        if (!mutable_path(filesystem_body)) {
            error(response, 400, "Invalid filesystem mutation path");
            return;
        }
        if (filesystem_upload_active()) {
            error(response, 409, "Filesystem upload is active");
            return;
        }
        filesystem::EntryType type{};
        std::uint32_t size{};
        const auto stat = filesystem::stat(filesystem_body, type, size);
        if (stat == filesystem::ReadResult::unavailable) {
            error(response, 503, "Filesystem unavailable");
            return;
        }
        if (stat != filesystem::ReadResult::succeeded) {
            error(response, stat_error_status(stat), "Filesystem entry unavailable");
            return;
        }
        if (type == filesystem::EntryType::directory) {
            bool has_entry = false;
            const auto stop_after_first = [](void* const context,
                                              const filesystem::DirectoryEntry&) {
                *static_cast<bool*>(context) = true;
                return false;
            };
            const auto listed = filesystem::list(filesystem_body, stop_after_first, &has_entry);
            if (listed == filesystem::DirectoryResult::output_stopped && has_entry) {
                error(response, 409, "Directory is not empty");
                return;
            }
            if (listed != filesystem::DirectoryResult::listed) {
                error(response, listed == filesystem::DirectoryResult::unavailable ? 503 : 500,
                    "Directory contents unavailable");
                return;
            }
        }
        if (filesystem::remove(filesystem_body) != filesystem::MutationResult::succeeded) {
            error(response, 500, "Filesystem delete failed");
            return;
        }
        (void)output.write("{}");
        return;
    }
    if (post && path == "/api/filesystem/upload/begin") {
        const auto separator = filesystem_body.find('\n');
        std::size_t total{};
        const auto target = separator == std::string_view::npos
                                ? std::string_view{}
                                : filesystem_body.substr(separator + 1);
        if (separator == std::string_view::npos ||
            !parse_decimal_size(filesystem_body.substr(0, separator), total) ||
            total > 0x7fffffffU || !mutable_path(target)) {
            error(response, 400, "Invalid filesystem upload path or size");
            return;
        }
        if (!filesystem::is_mounted(filesystem::volume(target))) {
            error(response, 503, "Filesystem unavailable");
            return;
        }
        if (filesystem_upload_active()) {
            error(response, 409, "Filesystem upload is active");
            return;
        }
        filesystem::EntryType existing_type{};
        std::uint32_t existing_size{};
        const auto existing = filesystem::stat(target, existing_type, existing_size);
        if (existing != filesystem::ReadResult::succeeded &&
            existing != filesystem::ReadResult::not_found) {
            error(response, stat_error_status(existing), "Upload target inspection failed");
            return;
        }
        if (existing == filesystem::ReadResult::succeeded &&
            existing_type != filesystem::EntryType::file) {
            error(response, 409, "Upload target is a directory");
            return;
        }
        std::array<char, 33> id{};
        if (!token(id)) {
            error(response, 503, "Upload identity unavailable");
            return;
        }
        if (filesystem::ensure_directory(upload_staging_directory(target)) !=
            filesystem::MutationResult::succeeded) {
            error(response, 500, "Upload staging unavailable");
            return;
        }
        // Create parent directories; preserve relative folder paths without buffering contents.
        for (std::size_t i = 1; i < target.size(); ++i) {
            if (target[i] == '/' && filesystem::ensure_directory(target.substr(0, i)) !=
                                        filesystem::MutationResult::succeeded) {
                error(response, 500, "Upload parent directory failed");
                return;
            }
        }
        if (filesystem::open_writer(upload_staging_file(target), filesystem_upload_.writer) !=
            filesystem::MutationResult::succeeded) {
            error(response, 507, "Upload staging failed");
            return;
        }
        filesystem_upload_.id = id;
        if (current != nullptr)
            filesystem_upload_.owner_session = current->token;
        std::memcpy(filesystem_upload_.path.data(), target.data(), target.size());
        filesystem_upload_.total = total;
        filesystem_upload_.last_activity = now;
        (void)output.format("{\"upload_id\":\"%s\",\"chunk_size\":1800,\"exists\":%s}", id.data(),
            existing == filesystem::ReadResult::succeeded ? "true" : "false");
        return;
    }
    if (post &&
        (path == "/api/filesystem/upload/chunk" || path == "/api/filesystem/upload/finish" ||
            path == "/api/filesystem/upload/abort")) {
        const bool chunk = path == "/api/filesystem/upload/chunk";
        if (!filesystem_upload_active() || filesystem_body.size() < 32 ||
            !equal_secret(filesystem_body.substr(0, 32), filesystem_upload_.id.data())) {
            error(response, 404, "Filesystem upload not found");
            return;
        }
        const bool same_owner =
            auth_required
                ? current != nullptr &&
                      equal_secret(filesystem_upload_.owner_session.data(), current->token.data())
                : filesystem_upload_.owner_session[0] == '\0';
        if (!same_owner) {
            error(response, 403, "Filesystem upload belongs to another session");
            return;
        }
        if (chunk) {
            std::size_t offset{}, end{}, total{};
            if (filesystem_body.size() <= 33 || filesystem_body[32] != '\n' ||
                filesystem_body.size() - 33 > 1800 ||
                !parse_content_range(request.content_range.data(), offset, end, total) ||
                total != filesystem_upload_.total || offset != filesystem_upload_.received ||
                end >= total || end - offset + 1 != filesystem_body.size() - 33) {
                clear_filesystem_upload();
                error(response, 400, "Invalid filesystem upload range");
                return;
            }
            if (filesystem::write(filesystem_upload_.writer, filesystem_body.data() + 33,
                    filesystem_body.size() - 33) != filesystem::MutationResult::succeeded) {
                clear_filesystem_upload();
                error(response, 507, "Filesystem write failed");
                return;
            }
            filesystem_upload_.received += filesystem_body.size() - 33;
            filesystem_upload_.last_activity = now;
            (void)output.format("{\"received_bytes\":%lu}",
                static_cast<unsigned long>(filesystem_upload_.received));
            return;
        }
        if (filesystem_body.size() != 32) {
            error(response, 400, "Invalid upload identity");
            return;
        }
        if (path == "/api/filesystem/upload/abort") {
            clear_filesystem_upload();
            (void)output.write("{}");
            return;
        }
        if (filesystem_upload_.received != filesystem_upload_.total) {
            error(response, 409, "Filesystem upload is incomplete");
            return;
        }
        const char* const staging = upload_staging_file(filesystem_upload_.path.data());
        const bool closed = filesystem::close(filesystem_upload_.writer);
        const bool renamed =
            closed && filesystem::rename(staging, filesystem_upload_.path.data()) ==
                          filesystem::MutationResult::succeeded;
        (void)filesystem::remove_if_exists(staging);
        filesystem_upload_ = {};
        if (!renamed) {
            error(response, 507, "Filesystem commit failed");
            return;
        }
        (void)output.write("{}");
        return;
    }
#endif
#if USE_BERRY && USE_WEB_CONSOLE
    if (berry_upload_begin_request) {
        if (berry_ == nullptr || !berry_->ready()) {
            error(response, 503, "Berry is unavailable");
            return;
        }
        std::size_t total = 0U;
        if (!parse_decimal_size({request.body.data(), request.body_size}, total) || total == 0U) {
            error(response, 400, "Invalid Berry source size");
            return;
        }
        if (total > kBerryConsoleMaxSourceBytes) {
            error(response, 413, "Berry source exceeds 64 KiB");
            return;
        }
        if (berry_upload_.source != nullptr) {
            const bool same_owner =
                auth_required
                    ? current != nullptr &&
                          equal_secret({berry_upload_.owner_session.data(),
                                           std::strlen(berry_upload_.owner_session.data())},
                              current->token.data())
                    : berry_upload_.owner_session[0] == '\0';
            if (!same_owner) {
                error(response, 409, "Another Berry upload is active");
                return;
            }
            clear_berry_upload();
        }

        const auto capacity = total + kBerryConsoleBufferOverhead;
        char* const source = new (std::nothrow) char[capacity];
        if (source == nullptr) {
            error(response, 507, "Not enough memory for Berry source");
            return;
        }
        std::array<char, 33> upload_id{};
        if (!token(upload_id)) {
            delete[] source;
            error(response, 503, "Upload identity unavailable");
            return;
        }
        berry_upload_.source = source;
        berry_upload_.id = upload_id;
        if (auth_required && current != nullptr)
            berry_upload_.owner_session = current->token;
        berry_upload_.total = total;
        berry_upload_.received = 0U;
        berry_upload_.last_activity = now;

        response.content_type = "application/json";
        (void)output.format("{\"upload_id\":\"%s\",\"chunk_size\":%lu}", berry_upload_.id.data(),
            static_cast<unsigned long>(kBerryUploadChunkSize));
        return;
    }

    if (berry_chunk_request) {
        const std::string_view envelope{request.body.data(), request.body_size};
        const auto separator = envelope.find('\n');
        if (separator != kBerryUploadIdLength || request.body_size <= kBerryUploadEnvelopeSize) {
            error(response, 400, "Invalid Berry upload chunk envelope");
            return;
        }
        if (berry_upload_.source == nullptr ||
            !equal_secret(envelope.substr(0U, separator), berry_upload_.id.data())) {
            error(response, 404, "Berry upload not found");
            return;
        }
        const bool same_owner =
            auth_required ? current != nullptr &&
                                equal_secret({berry_upload_.owner_session.data(),
                                                 std::strlen(berry_upload_.owner_session.data())},
                                    current->token.data())
                          : berry_upload_.owner_session[0] == '\0';
        if (!same_owner) {
            error(response, 403, "Berry upload belongs to another session");
            return;
        }

        std::size_t offset = 0U, end = 0U, total = 0U;
        const auto chunk_size = request.body_size - kBerryUploadEnvelopeSize;
        if (!parse_content_range(request.content_range.data(), offset, end, total) ||
            chunk_size == 0U || chunk_size > kBerryUploadChunkSize ||
            total != berry_upload_.total || offset != berry_upload_.received || end >= total ||
            end - offset + 1U != chunk_size) {
            clear_berry_upload();
            error(response, 400, "Invalid Berry upload range");
            return;
        }
        std::memcpy(berry_upload_.source + offset, request.body.data() + kBerryUploadEnvelopeSize,
            chunk_size);
        berry_upload_.received += chunk_size;
        berry_upload_.last_activity = now;
        response.content_type = "application/json";
        (void)output.format("{\"received_bytes\":%lu,\"total_bytes\":%lu}",
            static_cast<unsigned long>(berry_upload_.received),
            static_cast<unsigned long>(berry_upload_.total));
        return;
    }

    if (berry_upload_control_request) {
        const std::string_view upload_id{request.body.data(), request.body_size};
        if (upload_id.size() != kBerryUploadIdLength || berry_upload_.source == nullptr ||
            !equal_secret(upload_id, berry_upload_.id.data())) {
            error(response, 404, "Berry upload not found");
            return;
        }
        const bool same_owner =
            auth_required ? current != nullptr &&
                                equal_secret({berry_upload_.owner_session.data(),
                                                 std::strlen(berry_upload_.owner_session.data())},
                                    current->token.data())
                          : berry_upload_.owner_session[0] == '\0';
        if (!same_owner) {
            error(response, 403, "Berry upload belongs to another session");
            return;
        }
        if (path == "/api/berry/console/abort") {
            clear_berry_upload();
            response.content_type = "text/plain; charset=utf-8";
            (void)output.write("Aborted");
            return;
        }
        if (berry_upload_.received != berry_upload_.total) {
            error(response, 409, "Berry upload is incomplete");
            return;
        }
        if (berry_ == nullptr || !berry_->ready()) {
            error(response, 503, "Berry is unavailable");
            return;
        }

        auto* const source = berry_upload_.source;
        berry_upload_.source = nullptr;
        const auto source_size = berry_upload_.total;
        const auto source_capacity = source_size + kBerryConsoleBufferOverhead;
        clear_berry_upload();
        const auto result = berry_->repl_owned(source, source_size, source_capacity);
        if (result == BerryReplResult::invalid) {
            error(response, 400, "Invalid Berry source");
            return;
        }
        if (result == BerryReplResult::unavailable) {
            error(response, 503, "Berry is unavailable");
            return;
        }
        if (result == BerryReplResult::no_memory) {
            error(response, 503, "Out of memory");
            return;
        }
        response.content_type = "text/plain; charset=utf-8";
        (void)output.write("Accepted");
        return;
    }
#endif
    if (!post && path == "/api/session") {
        (void)output.write("{\"csrf\":");
        if (auth_required) {
            (void)output.json_string(current->csrf.data());
        } else {
            (void)output.write("\"\"");
        }
        (void)output.write(",\"auth_required\":");
        (void)output.write(auth_required ? "true" : "false");
        (void)output.write(",\"target\":");
        (void)output.json_string(platform::info().target());
        (void)output.write(",\"version\":");
        (void)output.json_string(platform::info().framework_version());
        (void)output.write("}");
    } else if (path == "/api/mqtt" || path == "/api/mqtt/preview" || path == "/api/mqtt/status" ||
               path == "/api/mqtt/test") {
#if USE_MQTT
        if (path == "/api/mqtt" && !post) {
            if (!write_mqtt_settings(output, settings_))
                error(response, 507, "MQTT settings exceeded response limit");
        } else if (post && (path == "/api/mqtt" || path == "/api/mqtt/preview")) {
            auto candidate = settings_.mqtt_settings();
            auto tele_period = settings_.tele_period_seconds();
            if (!parse_mqtt_form(
                    {request.body.data(), request.body_size}, candidate, tele_period)) {
                error(response, 400, "Invalid MQTT form");
                return;
            }
            if (candidate.secure) {
                error(response, 400, "TLS MQTT is unavailable");
                return;
            }
            // Resolve every topic before committing; errors leave all settings unchanged.
            if (!write_mqtt_preview(output, candidate, network_)) {
                error(response, 400, "MQTT topic expansion failed");
                return;
            }
            if (path == "/api/mqtt/preview")
                return;
            const auto result = settings_.set_mqtt_configuration(candidate, tele_period);
            if (result == SettingsUpdateResult::invalid) {
                error(response, 400, "Invalid MQTT settings");
                return;
            }
            if (result == SettingsUpdateResult::failed) {
                error(response, 507, "MQTT settings could not be saved");
                return;
            }
            response.size = 0U;
            (void)output.write("{\"saved\":true}");
        } else if (!post && path == "/api/mqtt/status") {
            const auto& stored = settings_.mqtt_settings();
            const auto state =
                mqtt_client_ == nullptr ? MqttTransportState::unavailable : mqtt_client_->state();
            const char* name = !stored.enabled                            ? "disabled"
                               : stored.secure                            ? "tls_unavailable"
                               : state == MqttTransportState::connected   ? "connected"
                               : state == MqttTransportState::connecting  ? "connecting"
                               : state == MqttTransportState::unavailable ? "unavailable"
                                                                          : "disconnected";
            (void)output.format(
                "{\"state\":\"%s\",\"tls_supported\":false,\"test_allowed\":%s,\"last_error\":",
                name, mqtt_test_allowed_ ? "true" : "false");
            (void)output.json_string(
                mqtt_client_ == nullptr ? std::string_view{} : mqtt_client_->last_error());
            if (mqtt_client_ != nullptr) {
                const auto stats = mqtt_client_->stats();
                (void)output.write(",\"test_error\":");
                (void)output.json_string(mqtt_client_->test_error());
                const auto test = mqtt_client_->connection_test_status();
                const char* test_name = test.state == MqttConnectionTestState::running  ? "running"
                                        : test.state == MqttConnectionTestState::passed ? "passed"
                                        : test.state == MqttConnectionTestState::failed ? "failed"
                                                                                        : "idle";
                (void)output.format(
                    ",\"connect_attempts\":%lu,\"connections\":%lu,\"reconnects\":%lu",
                    static_cast<unsigned long>(stats.connect_attempts),
                    static_cast<unsigned long>(stats.connections),
                    static_cast<unsigned long>(stats.reconnects));
                (void)output.format(",\"protocol_errors\":%lu,\"transport_errors\":%lu,\"publish_"
                                    "rejected\":%lu,\"test\":\"%s\"",
                    static_cast<unsigned long>(stats.protocol_errors),
                    static_cast<unsigned long>(stats.transport_errors),
                    static_cast<unsigned long>(stats.publish_rejected), test_name);
            }
            if (mqtt_service_ != nullptr) {
                const auto policy = mqtt_service_->stats();
                (void)output.format(",\"published\":%lu,\"publish_failures\":%lu,\"cleanup_"
                                    "failures\":%lu,\"result_rejected\":%lu",
                    static_cast<unsigned long>(policy.published),
                    static_cast<unsigned long>(policy.failed),
                    static_cast<unsigned long>(policy.cleanup_failures),
                    static_cast<unsigned long>(policy.result_rejected));
            }
            (void)output.write("}");
            if (output.failed())
                error(response, 507, "MQTT status exceeded response limit");
        } else if (post && path == "/api/mqtt/test") {
            if (request.body_size != 0U) {
                error(response, 400, "Broker test takes no settings");
                return;
            }
            if (!mqtt_test_allowed_) {
                error(response, 409, "MQTT stream is busy with an update");
                return;
            }
            const auto& stored = settings_.mqtt_settings();
            if (!stored.enabled || stored.secure || mqtt_client_ == nullptr) {
                error(response, 409, "Save and enable a plain MQTT broker first");
                return;
            }
            if (!mqtt_client_->start_connection_test(now)) {
                error(response, 409, "MQTT test is unavailable or already running");
                return;
            }
            response.status = 202U;
            (void)output.write("{\"test\":\"running\"}");
        } else
            error(response, 404, "Endpoint not found");
#else
        error(response, 404, "MQTT is unavailable in this image");
#endif
    } else if (!post && path == "/api/onboarding/status") {
        if (network_ == nullptr || (!onboarding && !recovery)) {
            error(response, 404, "Endpoint not found");
            return;
        }
        (void)write_provisioning_state(output, *network_, settings_, request);
    } else if (post && path == "/api/onboarding/apply") {
        if (network_ == nullptr || (!onboarding && !recovery)) {
            error(response, 404, "Endpoint not found");
            return;
        }
        ProvisioningForm form{};
        if (!parse_provisioning_form({request.body.data(), request.body_size}, form)) {
            error(response, 400, "Invalid Wi-Fi form");
            return;
        }
        const auto result = network_->start_provisioning_test({form.ssid.data(), form.ssid_size},
            {form.password.data(), form.password_size},
            {form.web_password.data(), form.web_password_size}, now);
        if (result == platform::NetworkResult::success) {
            response.status = 202;
            (void)output.write("{\"state\":\"testing\"}");
        } else if (result == platform::NetworkResult::busy) {
            error(response, 409,
                network_->wifi_scan_pending() ? "Wi-Fi scan is still running"
                                              : "Wi-Fi test already running");
        } else if (result == platform::NetworkResult::unavailable) {
            error(response, 503, "Wi-Fi test unavailable");
        } else {
            error(response, 400, "Invalid Wi-Fi credentials");
        }
#if USE_WEB_FILESYSTEM
    } else if (post && path == "/api/filesystem/list") {
        FilesystemListRequest list_request{};
        if (!parse_filesystem_list_request(
                {request.body.data(), request.body_size}, list_request)) {
            error(response, 400, "Invalid filesystem path or cursor");
            return;
        }

        response.content_type = "application/json";
        if (!output.write("{\"path\":") || !output.json_string(list_request.path) ||
            !output.write(",\"entries\":[")) {
            error(response, 507, "Filesystem listing exceeded response limit");
            return;
        }

        FilesystemListOutput page{&output, list_request.offset, list_request.offset};
        const auto result = filesystem::list(list_request.path, write_filesystem_entry, &page);
        if (result == filesystem::DirectoryResult::unavailable) {
            error(response, 503, "Filesystem unavailable");
            return;
        }
        if (result == filesystem::DirectoryResult::invalid_path) {
            error(response, 400, "Invalid filesystem path");
            return;
        }
        if (result == filesystem::DirectoryResult::open_failed) {
            error(response, 404, "Directory not found");
            return;
        }
        if (result == filesystem::DirectoryResult::read_failed) {
            error(response, 500, "Directory read failed");
            return;
        }
        if (page.too_large || output.failed() ||
            (result == filesystem::DirectoryResult::output_stopped && !page.more)) {
            error(response, 507, "Filesystem listing exceeded response limit");
            return;
        }

        if (!output.write("],\"next\":")) {
            error(response, 507, "Filesystem listing exceeded response limit");
            return;
        }
        if (page.more) {
            if (!output.format("%lu,\"more\":true}", static_cast<unsigned long>(page.next))) {
                error(response, 507, "Filesystem listing exceeded response limit");
            }
        } else if (!output.write("null,\"more\":false}")) {
            error(response, 507, "Filesystem listing exceeded response limit");
        }
#endif
    } else if (!post && path == "/api/info") {
        const auto& identity = platform::info();
        const auto network = platform::network_info();
        const auto access_point = platform::access_point_info();
        const auto& local_network =
            network.address[0] != '\0' && !support::ascii_iequals(network.address, "0.0.0.0")
                ? network
                : access_point;
        const auto memory = platform::memory_info();
        const auto runtime_hostname =
            network_ == nullptr ? std::string_view{local_network.hostname} : network_->hostname();
        filesystem::SpaceInfo space{};
        const bool filesystem_available = filesystem::space(space);
        (void)output.write("{\"identity\":{\"target\":");
        (void)output.json_string(identity.target());
        (void)output.write(",\"chip\":");
        (void)output.json_string(identity.chip());
        (void)output.write(",\"sdk\":");
        (void)output.json_string(identity.sdk_name());
        (void)output.write(",\"sdk_version\":");
        (void)output.json_string(identity.sdk_version());
        (void)output.write(",\"version\":");
        (void)output.json_string(identity.framework_version());
        (void)output.write(",\"device_name\":");
        (void)output.json_string(settings_.device_name());
        (void)output.write("},\"restart_required\":");
        (void)output.write(settings_.restart_required() ? "true" : "false");
        (void)output.write(",\"network\":{\"hostname\":");
        (void)output.json_string(
            runtime_hostname.empty() ? std::string_view{local_network.hostname} : runtime_hostname);
        (void)output.write(",\"address\":");
        (void)output.json_string(local_network.address);
        (void)output.write(",\"gateway\":");
        (void)output.json_string(local_network.gateway);
        (void)output.write(",\"subnet\":");
        (void)output.json_string(local_network.subnet);
        (void)output.write(",\"mac\":");
        (void)output.json_string(local_network.mac);
        (void)output.write(",\"ssid\":");
#if USE_TASMOTA_STATE
        // The connected network, as Tasmota's AP Information shows it; the saved SSId1 otherwise.
        (void)output.json_string(network_ != nullptr && network_->connected()
                                     ? network_->station_ssid()
                                     : settings_.ssid(0));
#else
        (void)output.json_string(settings_.ssid(0));
#endif
#if USE_WIFI
        platform::WifiLink link{};
        if (platform::wifi_link(link) && link.channel != 0U)
            (void)output.format(",\"channel\":%u,\"band\":\"%s\"",
                static_cast<unsigned>(link.channel), platform::wifi_band(link.channel));
#endif
        (void)output.write("},\"memory\":{");
        (void)output.format("\"heap_free\":%lu,\"stack_low_watermark\":%lu,\"flash\":%lu,"
                            "\"program\":%lu,\"program_capacity\":%lu",
            static_cast<unsigned long>(memory.heap_free_bytes),
            static_cast<unsigned long>(memory.stack_low_watermark_bytes),
            static_cast<unsigned long>(memory.flash_size_bytes),
            static_cast<unsigned long>(memory.program_size_bytes),
            static_cast<unsigned long>(memory.program_capacity_bytes));
#ifdef NEWMOTA_PSRAM_MODE
        (void)output.write(",\"psram_mode\":\"" NEWMOTA_PSRAM_MODE "\"");
#endif
        (void)output.write("},\"operation\":{\"boot_count\":");
        const auto boot = boot_counter_ == nullptr ? BootStatus{} : boot_counter_->status();
        if (boot.available) {
            (void)output.format("%lu", static_cast<unsigned long>(boot.count));
        } else {
            (void)output.write("null");
        }
        if (boot_guard().reduced())
            (void)output.format(
                ",\"boot_loop\":%u", static_cast<unsigned int>(boot_guard().restart().count));
        (void)output.write("},\"filesystem\":{\"mounted\":");
        (void)output.write(filesystem::is_mounted() ? "true" : "false");
        (void)output.format(",\"available\":%s,\"used\":%llu,\"total\":%llu}",
            filesystem_available ? "true" : "false",
            static_cast<unsigned long long>(space.used_bytes),
            static_cast<unsigned long long>(space.total_bytes));
#if USE_SDCARD
        // Present in USE_SDCARD builds: the browser shows the card beside the flash volume.
        filesystem::SpaceInfo card{};
        const bool card_available = filesystem::space(card, filesystem::Volume::sd);
        (void)output.format(",\"sd\":{\"mounted\":%s,\"available\":%s,\"used\":%llu,"
                            "\"total\":%llu}",
            filesystem::is_mounted(filesystem::Volume::sd) ? "true" : "false",
            card_available ? "true" : "false", static_cast<unsigned long long>(card.used_bytes),
            static_cast<unsigned long long>(card.total_bytes));
#endif
        (void)output.write(",\"web\":{\"secure\":");
        (void)output.write(request.secure ? "true" : "false");
        (void)output.write(",\"transport\":");
        (void)output.json_string(request.secure ? "HTTPS" : "HTTP");
        (void)output.write(",\"encoding\":");
        (void)output.json_string(kWebAssetEncodingLabel);
        (void)output.write("}");
#if USE_WEB_AUDIO
        // The player card appears once the template has an I2S output.
        if (i2s::Config tx{}; i2s::configured_config(tx, platform::i2s::Direction::transmit))
            (void)output.write(",\"audio\":true");
#endif
#if USE_WEBCAM || USE_CSI_WEBCAM
        (void)output.write(camera::configured() ? ",\"camera\":true}" : "}");
#else
        (void)output.write("}");
#endif
    } else if (!post && path == "/api/state") {
        const auto* const state = messages_.latest_snapshot("power");
        if (state == nullptr || !output.write(state->payload())) {
            error(response, 503, "State unavailable");
            return;
        }
    } else if (!post && path == "/api/sensors") {
        const auto* const sensors = messages_.latest_snapshot("sensors");
        if (sensors == nullptr || !output.write(sensors->payload())) {
            error(response, 503, "Sensor telemetry unavailable");
            return;
        }
    } else if (!post && path == "/api/sensors/readings") {
#if USE_SENSORS
        if (!sensor_service().write_readings(output)) {
            error(response, 507, "Sensor readings exceeded response limit");
            return;
        }
#else
        (void)output.write("{\"readings\":[]}");
#endif
#if USE_SENSORS
    } else if (!post && (path == "/api/sensors/updates" || path == "/api/sensors/updates/all")) {
        // The dashboard's question: anything new? Everything is marked on page load.
        static constexpr std::size_t kTailRoom = 64;
        static constexpr std::uint32_t kMetricsPeriod = 10000;
        static std::uint32_t metrics_at = now - kMetricsPeriod;
        const auto viewer = current == nullptr ? 0U : std::size_t(current - sessions_.data());
        bool more = false, reset = false;
        if (!output.write("{") || !sensor_service().write_updates(viewer, path.size() > 20, output,
                                      response.body.size() - kTailRoom, more, reset)) {
            error(response, 507, "Sensor updates exceeded response limit");
            return;
        }
        const auto member = [&](const char* name) {
            return (output.size() > 1 ? output.write(",") : true) && output.write(name);
        };
        if (more)
            (void)member("\"more\":true");
        if (reset)
            (void)member("\"reset\":true");
        if (now - metrics_at >= kMetricsPeriod) {
            metrics_at = now;
            if (member("\"heap\":"))
                (void)output.format(
                    "%lu", static_cast<unsigned long>(platform::memory_info().heap_free_bytes));
        }
        (void)output.write("}");
    } else if (!post && path.substr(0, 21) == "/api/sensors/history/") {
        std::uint32_t series = 0;
        if (!support::parse_unsigned_decimal(path.substr(21), UINT32_MAX, series) ||
            !sensor_history().write_series(series, output)) {
            error(response, 404, "Sensor history unavailable");
            return;
        }
#endif
#if USE_WEB_AUDIO
    } else if (!post && path == "/api/audio") {
        if (!audio_web::write_player(output)) {
            error(response, 507, "Audio status exceeded response limit");
            return;
        }
#endif
#if USE_WEBCAM || USE_CSI_WEBCAM
    } else if (!post && path == "/api/camera") {
        // The Home card embeds the port-81 page; frames never pass through this server. Each
        // server start drops its viewers, so the card reloads on a new count.
        const auto& status = camera::status();
        const bool stream = status.phase == camera::Phase::streaming && platform::camera::serving();
        if (!output.format("{\"stream\":%s,\"port\":%u,\"width\":%u,\"height\":%u,"
                           "\"starts\":%lu,\"sensor\":",
                stream ? "true" : "false", unsigned(camera::kStreamPort),
                unsigned(status.mode.width), unsigned(status.mode.height),
                static_cast<unsigned long>(platform::camera::serves())) ||
            !output.json_string(status.sensor.data())) {
            error(response, 507, "Camera status exceeded response limit");
            return;
        }
#if USE_CSI_WEBCAM
        // The card's direction pad moves the window with WcPosition.
        const auto at = camera::position(status.mode);
        if (stream && camera::pannable(status.mode) &&
            !output.format(",\"pan\":%d,\"tilt\":%d", at.pan, at.tilt)) {
            error(response, 507, "Camera status exceeded response limit");
            return;
        }
#endif
        if (!output.write("}")) {
            error(response, 507, "Camera status exceeded response limit");
            return;
        }
#endif
#if USE_BERRY
    } else if (!post && path == "/api/widgets") {
        widgets::note_viewer(now);
        if (!widgets::write_index(output)) {
            error(response, 507, "Widget index exceeded response limit");
            return;
        }
    } else if (post && path == "/api/widgets/action") {
        // The script sees the result through the widget's next revision, not in this reply.
        switch (widgets::enqueue_action({request.body.data(), request.body_size})) {
        case widgets::ActionResult::queued:
            (void)output.write("{\"queued\":true}");
            break;
        case widgets::ActionResult::invalid:
            error(response, 400, "Invalid widget action");
            break;
        case widgets::ActionResult::unknown:
            error(response, 404, "Widget unavailable");
            break;
        case widgets::ActionResult::full:
            error(response, 503, "Widget action queue full");
            break;
        }
    } else if (!post && path.substr(0, 13) == "/api/widgets/") {
        if (!widgets::write_widget(path.substr(13), output)) {
            error(response, 404, "Widget unavailable");
            return;
        }
#endif
#if USE_WEB_CONSOLE
    } else if (post && path == "/api/console/poll") {
        ConsoleCursors cursors{};
        if (!parse_console_cursors({request.body.data(), request.body_size}, cursors)) {
            error(response, 400, "Invalid console cursor");
            return;
        }
        if (console_history_generation_[0] == '\0' && !token(console_history_generation_)) {
            error(response, 503, "Console history identity unavailable");
            return;
        }
        if (!console_history::attach_viewer()) {
            error(response, 503, "Out of memory");
            return;
        }
        response.content_type = "application/json";
        bool berry_available = false;
#if USE_BERRY
        berry_available = berry_ != nullptr && berry_->ready();
#endif
        if (!output.write("{\"generation\":") ||
            !output.json_string({console_history_generation_.data(), 32U}) ||
            !output.write(",\"berry_available\":") ||
            !output.write(berry_available ? "true" : "false") || !output.write(",\"web\":") ||
            !write_console_batch(output, false, cursors.web) || !output.write(",\"berry\":") ||
            !write_console_batch(output, true, cursors.berry) || !output.write("}")) {
            error(response, 507, "Console output exceeded response limit");
        }
    } else if (post && path == "/api/berry/console") {
#if USE_BERRY
        if (berry_ == nullptr || !berry_->ready()) {
            error(response, 503, "Berry is unavailable");
            return;
        }
        const auto result = berry_->repl({request.body.data(), request.body_size});
        if (result == BerryReplResult::invalid) {
            error(response, 400, "Invalid Berry source");
            return;
        }
        if (result == BerryReplResult::unavailable) {
            error(response, 503, "Berry is unavailable");
            return;
        }
        if (result == BerryReplResult::no_memory) {
            error(response, 503, "Out of memory");
            return;
        }
        response.content_type = "text/plain; charset=utf-8";
        (void)output.write("Accepted");
#else
        error(response, 503, "Berry support is unavailable");
#endif
#endif
    } else if (post && path == "/api/logout") {
        if (filesystem_upload_active() &&
            (!auth_required ||
                (current != nullptr &&
                    equal_secret(filesystem_upload_.owner_session.data(), current->token.data()))))
            clear_filesystem_upload();
#if USE_BERRY
        if (berry_upload_.source != nullptr &&
            (!auth_required ||
                (current != nullptr &&
                    equal_secret({berry_upload_.owner_session.data(),
                                     std::strlen(berry_upload_.owner_session.data())},
                        current->token.data())))) {
            clear_berry_upload();
        }
#endif
        if (current != nullptr)
            *current = {};
        std::snprintf(response.cookie.data(), response.cookie.size(),
            "nm_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0%s",
            request.secure ? "; Secure" : "");
        (void)output.write("{}");
    } else if (post &&
               (path == "/api/command" || (kWebConsole && path == "/api/console/command"))) {
        const bool console_command = kWebConsole && path == "/api/console/command";
        const std::string_view command{request.body.data(), request.body_size};
        if (command.empty() || command.size() > kWebCommandSourceCapacity ||
            command.find_first_of("\r\n") != std::string_view::npos ||
            command.find('\0') != std::string_view::npos) {
            error(response, command.size() > kWebCommandSourceCapacity ? 413 : 400,
                command.size() > kWebCommandSourceCapacity ? "Command exceeds the 512-byte limit"
                                                           : "Invalid command");
            return;
        }
        if (console_command && console_history_generation_[0] == '\0' &&
            !token(console_history_generation_)) {
            error(response, 503, "Console history identity unavailable");
            return;
        }
        // Console output may be multiple JSON records; preserve it as plain text.
        response.content_type = "text/plain; charset=utf-8";
        std::uint32_t group = 0U;
#if USE_TASMOTA_STATE
        TimestampedOutput stamped_response{append_console_command, &response};
        CommandOutput console_output{TimestampedOutput::write, &stamped_response};
#else
        CommandOutput console_output{append_console_command, &response};
#endif
        CommandOutput& command_output = console_command ? console_output : output;
        const auto result = commands_.execute(command, CommandSource::web_console, command_output,
            console_command ? &group : nullptr);
        if (result.output_failed) {
            response.content_type = "application/json";
            error(
                response, 507, "Response exceeded limit; command may have executed; do not retry");
        } else if (console_command && response.size != 0U && group != 0U) {
            if (!output.format("\n__NEWMOTA_RESULT_%08lx_%s__", static_cast<unsigned long>(group),
                    console_history_generation_.data())) {
                response.content_type = "application/json";
                error(response, 507,
                    "Response exceeded limit; command may have executed; do not retry");
            }
        } else if (response.size == 0) {
            (void)output.write("Accepted");
        }
    } else {
        error(response, 404, "Endpoint not found");
    }
}

} // namespace newmota
