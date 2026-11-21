/*
  settings_commands.cpp - settings commands for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
  MaxPower/SO33 reference: Tasmota xdrv_03_esp32_energy.ino, settings.ino and
  support_command.ino, Copyright (C) Theo Arends, GPL version 3 or later.
*/

/** @file settings_commands.cpp
 * @brief Persistent settings commands and prospective accounting policy changes.
 */

#include "services/settings/settings_commands.h"

#include "newmota_features.h"
#if USE_ENERGY
#include "services/energy/energy_service.h"
#endif
#if USE_SENSORS
#include "drivers/sensors/sensor_service.h"
#endif
#if USE_BLE
#include "newmota/platform/ble.h"
#endif
#include "core/system/restart.h"
#include "newmota/log.h"
#include "services/settings/settings.h"
#include "newmota/platform/platform.h"
#include "services/power/power_service.h"
#if USE_WIFI_POWER
#include "services/network/wifi_tx_power.h"
#endif
#include "support/number.h"
#include "support/text.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace newmota::commands {
namespace {

/**
 * @brief Writes one SetOption response using its boolean or numeric form.
 */
[[nodiscard]] bool write_option(
    CommandOutput& output, const std::uint16_t index, const std::uint8_t value) {
    if (Settings::is_parameter_option(index)) {
        return output.format("{\"SetOption%u\":%u}", static_cast<unsigned int>(index),
            static_cast<unsigned int>(value));
    }
    return output.format(
        "{\"SetOption%u\":\"%s\"}", static_cast<unsigned int>(index), value == 0 ? "OFF" : "ON");
}

/**
 * @brief Maps a settings update outcome to the command status contract.
 */
[[nodiscard]] CommandStatus update_status(const SettingsUpdateResult result) {
    switch (result) {
    case SettingsUpdateResult::pending:
    case SettingsUpdateResult::saved:
    case SettingsUpdateResult::unchanged:
        return CommandStatus::handled;
    case SettingsUpdateResult::invalid:
        return CommandStatus::invalid;
    case SettingsUpdateResult::failed:
        return CommandStatus::failed;
    }
    return CommandStatus::failed;
}

#if USE_SENSORS
/** @brief Queries compiled drivers or updates one named persistent probe policy. */
CommandStatus sensor_driver_command(
    const CommandRequest& request, Settings& settings, CommandOutput& output) {
    if (request.has_index)
        return CommandStatus::invalid;

    const auto& sensors = sensor_service();
    const SensorDriverDescriptor* selected = nullptr;
    auto name = support::trim_ascii(request.payload);
    auto state_text = std::string_view{};
    bool update = false;

    if (request.is_query()) {
        if (!output.write("{\"SensorDriver\":{"))
            return CommandStatus::failed;
        bool first = true;
        for (std::size_t i = 0; const auto* driver = sensors.driver(i); ++i) {
            if (!output.format("%s\"%u\":", first ? "" : ",", unsigned(driver->id)) ||
                !output.json_string(driver->name))
                return CommandStatus::failed;
            first = false;
        }
        return output.write("}}") ? CommandStatus::handled : CommandStatus::failed;
    }

    // A final state token is optional. Keeping the rest as the driver name
    // permits future registered names to contain spaces.
    const auto separator = name.find_last_of(" \t\r\n\f\v");
    if (separator != std::string_view::npos) {
        const auto suffix = support::trim_ascii(name.substr(separator + 1U));
        const auto parsed = parse_command_state(suffix);
        if (parsed != CommandState::invalid) {
            if (parsed != CommandState::off && parsed != CommandState::on)
                return CommandStatus::invalid;
            name = support::trim_ascii(name.substr(0U, separator));
            state_text = suffix;
            update = true;
        } else if (suffix == "?") {
            name = support::trim_ascii(name.substr(0U, separator));
        }
    }
    if (name.empty())
        return CommandStatus::invalid;

    for (std::size_t i = 0; const auto* driver = sensors.driver(i); ++i) {
        if (support::ascii_iequals(name, driver->name)) {
            selected = driver;
            break;
        }
    }
    if (!selected)
        return CommandStatus::invalid;

    if (update) {
        const auto state = parse_command_state(state_text);
        const auto status = update_status(
            settings.set_sensor_driver_enabled(selected->id, state == CommandState::on));
        if (status != CommandStatus::handled)
            return status;
        sensor_service().configure(settings.sensor_settings(), platform::monotonic_milliseconds());
    }
    if (!output.format("{\"SensorDriver\":{\"ID\":%u,\"Name\":", unsigned(selected->id)) ||
        !output.json_string(selected->name) || !output.write(",\"Field\":") ||
        !output.json_string(selected->output_field) || !output.write(",\"Routes\":") ||
        !output.json_string(selected->output_routes) ||
        !output.format(
            ",\"Enabled\":%u}}", settings.sensor_settings().enabled(selected->id) ? 1U : 0U))
        return CommandStatus::failed;
    return CommandStatus::handled;
}

/** @brief Parses a signed offset expressed in whole units or tenths. */
[[nodiscard]] bool parse_sensor_offset(std::string_view text, const std::int32_t minimum,
    const std::int32_t maximum, std::int16_t& value) {
    text = support::trim_ascii(text);
    if (text.empty())
        return false;
    bool negative = false;
    if (text.front() == '+' || text.front() == '-') {
        negative = text.front() == '-';
        text.remove_prefix(1U);
    }
    if (text.empty())
        return false;

    const auto point = text.find('.');
    const auto whole_text = text.substr(0U, point);
    std::uint32_t whole = 0U;
    if (!support::parse_unsigned_decimal(whole_text, 1000U, whole))
        return false;
    std::uint32_t fraction = 0U;
    if (point != std::string_view::npos) {
        const auto decimal = text.substr(point + 1U);
        if (decimal.size() != 1U || !support::parse_unsigned_decimal(decimal, 9U, fraction))
            return false;
    }
    const auto magnitude = static_cast<std::int32_t>(whole * 10U + fraction);
    const auto parsed = negative ? -magnitude : magnitude;
    if (parsed < minimum || parsed > maximum)
        return false;
    value = static_cast<std::int16_t>(parsed);
    return true;
}

enum class SensorOutputSetting : std::uint8_t {
    temperature_resolution,
    humidity_resolution,
    pressure_resolution,
    temperature_offset,
    humidity_offset,
};

/** @brief Handles one persistent precision or compensation command. */
CommandStatus sensor_output_command(const CommandRequest& request, Settings& settings,
    CommandOutput& output, const SensorOutputSetting selected) {
    if (request.has_index)
        return CommandStatus::invalid;
    auto value = settings.sensor_output_settings();
    if (!request.is_query()) {
        std::uint32_t resolution = 0U;
        std::int16_t offset = 0;
        switch (selected) {
        case SensorOutputSetting::temperature_resolution:
            if (!support::parse_unsigned_decimal(request.payload, 3U, resolution))
                return CommandStatus::invalid;
            value.temperature_resolution = static_cast<std::uint8_t>(resolution);
            break;
        case SensorOutputSetting::humidity_resolution:
            if (!support::parse_unsigned_decimal(request.payload, 3U, resolution))
                return CommandStatus::invalid;
            value.humidity_resolution = static_cast<std::uint8_t>(resolution);
            break;
        case SensorOutputSetting::pressure_resolution:
            if (!support::parse_unsigned_decimal(request.payload, 3U, resolution))
                return CommandStatus::invalid;
            value.pressure_resolution = static_cast<std::uint8_t>(resolution);
            break;
        case SensorOutputSetting::temperature_offset:
            if (!parse_sensor_offset(request.payload, -126, 126, offset))
                return CommandStatus::invalid;
            value.temperature_offset_tenths = static_cast<std::int8_t>(offset);
            break;
        case SensorOutputSetting::humidity_offset:
            if (!parse_sensor_offset(request.payload, -100, 100, offset))
                return CommandStatus::invalid;
            value.humidity_offset_tenths = static_cast<std::int8_t>(offset);
            break;
        }
        const auto status = update_status(settings.set_sensor_output_settings(value));
        if (status != CommandStatus::handled)
            return status;
        sensor_service().set_presentation(settings.sensor_presentation());
    }

    const auto& current = settings.sensor_output_settings();
    const char* key = "";
    std::uint8_t resolution = 0U;
    std::int16_t offset = 0;
    switch (selected) {
    case SensorOutputSetting::temperature_resolution:
        key = "TempRes";
        resolution = current.temperature_resolution;
        break;
    case SensorOutputSetting::humidity_resolution:
        key = "HumRes";
        resolution = current.humidity_resolution;
        break;
    case SensorOutputSetting::pressure_resolution:
        key = "PressRes";
        resolution = current.pressure_resolution;
        break;
    case SensorOutputSetting::temperature_offset:
        key = "TempOffset";
        offset = current.temperature_offset_tenths;
        break;
    case SensorOutputSetting::humidity_offset:
        key = "HumOffset";
        offset = current.humidity_offset_tenths;
        break;
    }
    if (selected == SensorOutputSetting::temperature_offset ||
        selected == SensorOutputSetting::humidity_offset) {
        const auto magnitude = static_cast<unsigned>(offset < 0 ? -offset : offset);
        return output.format(
                   "{\"%s\":%s%u.%u}", key, offset < 0 ? "-" : "", magnitude / 10U, magnitude % 10U)
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    }
    return output.format("{\"%s\":%u}", key, static_cast<unsigned>(resolution))
               ? CommandStatus::handled
               : CommandStatus::failed;
}
#endif

/**
 * @brief Writes one SSID response for a zero-based slot.
 */
[[nodiscard]] bool write_ssid(
    CommandOutput& output, const Settings& settings, const std::uint8_t slot) {
    return output.format("{\"SSId%u\":", static_cast<unsigned int>(slot + 1)) &&
           output.json_string(settings.ssid(slot)) && output.write("}");
}

/**
 * @brief Writes both configured SSIDs in one response.
 */
[[nodiscard]] bool write_all_ssids(CommandOutput& output, const Settings& settings) {
    return output.write("{\"SSId1\":") && output.json_string(settings.ssid(0)) &&
           output.write(",\"SSId2\":") && output.json_string(settings.ssid(1)) && output.write("}");
}

/** @brief Resolves Tasmota's clear/default credential payloads. */
[[nodiscard]] std::string_view wifi_credential_value(
    const std::string_view payload, const std::uint8_t slot, const bool password) {
    if (payload == "0") {
        return {};
    }
    if (payload == "1") {
        return password ? Settings::default_password(slot) : Settings::default_ssid(slot);
    }
    return payload;
}

/**
 * @brief Handles SSId queries, clearing, and updates.
 */
CommandStatus ssid_command(
    const CommandRequest& request, Settings& settings, CommandOutput& output) {
    if (!request.has_index) {
        if (!request.is_query()) {
            return CommandStatus::invalid;
        }
        return write_all_ssids(output, settings) ? CommandStatus::handled : CommandStatus::failed;
    }
    if (request.index == 0 || request.index > Settings::kWifiSlotCount) {
        return CommandStatus::invalid;
    }

    const auto slot = static_cast<std::uint8_t>(request.index - 1);
    if (!request.is_query()) {
        const auto value = wifi_credential_value(request.payload, slot, false);
        const auto status = update_status(settings.set_ssid(slot, value, true));
        if (status != CommandStatus::handled) {
            return status;
        }
    }
    return write_ssid(output, settings, slot) ? CommandStatus::handled : CommandStatus::failed;
}

/**
 * @brief Writes a masked password response for a zero-based slot.
 */
[[nodiscard]] bool write_password(CommandOutput& output, const std::uint8_t slot) {
    return output.format("{\"Password%u\":\"****\"}", static_cast<unsigned int>(slot + 1));
}

/**
 * @brief Handles Password queries, clearing, and updates.
 */
CommandStatus password_command(
    const CommandRequest& request, Settings& settings, CommandOutput& output) {
    if (request.has_index && (request.index == 0 || request.index > Settings::kWifiSlotCount)) {
        return CommandStatus::invalid;
    }

    const auto slot = static_cast<std::uint8_t>(request.has_index ? request.index - 1U : 0U);
    if (!request.is_query()) {
        const auto value = wifi_credential_value(request.payload, slot, true);
        const auto status = update_status(settings.set_password(slot, value, true));
        if (status != CommandStatus::handled) {
            return status;
        }
    }
    return write_password(output, slot) ? CommandStatus::handled : CommandStatus::failed;
}

/** @brief Writes and updates Tasmota's persisted preferred Wi-Fi slot. */
CommandStatus ap_command(const CommandRequest& request, Settings& settings, CommandOutput& output) {
    if (request.has_index) {
        return CommandStatus::invalid;
    }
    if (!request.is_query()) {
        std::uint32_t value = 0U;
        if (!support::parse_unsigned_decimal(request.payload, 2U, value)) {
            return CommandStatus::invalid;
        }
        const auto selected =
            value == 0U ? static_cast<std::uint8_t>(settings.preferred_wifi_slot() == 0U ? 1U : 0U)
                        : static_cast<std::uint8_t>(value - 1U);
        const auto status = update_status(settings.set_preferred_wifi_slot(selected));
        if (status != CommandStatus::handled) {
            return status;
        }
    }
    return output.format("{\"Ap\":%u,\"SSId\":",
               static_cast<unsigned int>(settings.preferred_wifi_slot() + 1U)) &&
                   output.json_string(settings.ssid(settings.preferred_wifi_slot())) &&
                   output.write("}")
               ? CommandStatus::handled
               : CommandStatus::failed;
}

#if USE_MQTT
/** @brief Writes one bounded MQTT text setting. */
[[nodiscard]] bool write_mqtt_text(
    CommandOutput& output, const char* const name, const std::string_view value) {
    return output.format("{\"%s\":", name) && output.json_string(value) && output.write("}");
}

/** @brief Writes the masked MQTT password setting. */
[[nodiscard]] bool write_mqtt_password(CommandOutput& output) {
    return output.write("{\"MqttPassword\":\"****\"}");
}

/** @brief Writes the current MQTT settings without exposing credentials. */
[[nodiscard]] bool write_mqtt_settings(CommandOutput& output, const Settings& settings) {
    const auto& mqtt = settings.mqtt_settings();
    return output.write("{\"MqttHost\":") && output.json_string(mqtt.host()) &&
           output.format(",\"MqttClient\":") && output.json_string(mqtt.client_id_template()) &&
           output.format(",\"MqttUser\":") && output.json_string(mqtt.username()) &&
           output.write(",\"MqttPassword\":\"****\"") && output.format(",\"MqttTopic\":") &&
           output.json_string(mqtt.topic()) &&
           output.format(",\"MqttPort\":%u,\"MqttKeepAlive\":%u,"
                         "\"MqttEnable\":\"%s\",\"MqttTLS\":\"%s\"}",
               static_cast<unsigned int>(mqtt.port),
               static_cast<unsigned int>(mqtt.keep_alive_seconds), mqtt.enabled ? "ON" : "OFF",
               mqtt.secure ? "ON" : "OFF");
}

/** @brief Handles bounded MQTT broker settings and enablement commands. */
CommandStatus mqtt_command(
    const CommandRequest& request, Settings& settings, CommandOutput& output) {
    const auto& current = settings.mqtt_settings();

    if (support::ascii_iequals(request.name, "mqtt")) {
        if (request.has_index)
            return CommandStatus::invalid;
        return request.is_query() ? (write_mqtt_settings(output, settings) ? CommandStatus::handled
                                                                           : CommandStatus::failed)
                                  : CommandStatus::invalid;
    }

    auto candidate = current;
    const auto name = request.name;
    const bool topic =
        support::ascii_iequals(name, "topic") || support::ascii_iequals(name, "mqtttopic");
    const bool full = support::ascii_iequals(name, "fulltopic");
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    const bool button_topic = support::ascii_iequals(name, "buttontopic");
    const bool button_retain = support::ascii_iequals(name, "buttonretain");
#endif
#if USE_SWITCHES
    const bool switch_topic = support::ascii_iequals(name, "switchtopic");
    const bool switch_retain = support::ascii_iequals(name, "switchretain");
#else
    constexpr bool switch_topic = false;
    [[maybe_unused]] constexpr bool switch_retain = false;
#endif
    const bool prefix = support::ascii_iequals(name, "prefix");
    const bool group = support::ascii_iequals(name, "grouptopic");
    const bool host = support::ascii_iequals(name, "mqtthost");
    const bool client = support::ascii_iequals(name, "mqttclient");
    const bool user = support::ascii_iequals(name, "mqttuser");
    const bool password = support::ascii_iequals(name, "mqttpassword");
    const bool port = support::ascii_iequals(name, "mqttport");
    const bool keep = support::ascii_iequals(name, "mqttkeepalive");
    const bool retry = support::ascii_iequals(name, "mqttretry");
    const bool timeout = support::ascii_iequals(name, "mqtttimeout");
    const bool enable = support::ascii_iequals(name, "mqttenable");
    const bool tls = support::ascii_iequals(name, "mqtttls");
    bool* state = nullptr;
    const char* key = nullptr;
    if (support::ascii_iequals(name, "powerretain")) {
        state = &candidate.power_retain;
        key = "PowerRetain";
    } else if (support::ascii_iequals(name, "inforetain")) {
        state = &candidate.info_retain;
        key = "InfoRetain";
    } else if (support::ascii_iequals(name, "stateretain")) {
        state = &candidate.state_retain;
        key = "StateRetain";
    } else if (support::ascii_iequals(name, "statusretain")) {
        state = &candidate.status_retain;
        key = "StatusRetain";
    }
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    else if (button_retain) {
        state = &candidate.button_retain;
        key = "ButtonRetain";
    } else if (button_topic)
        key = "ButtonTopic";
#endif
#if USE_SWITCHES
    else if (switch_retain) {
        state = &candidate.switch_retain;
        key = "SwitchRetain";
    } else if (switch_topic)
        key = "SwitchTopic";
#endif
    else if (host)
        key = "MqttHost";
    else if (client)
        key = "MqttClient";
    else if (user)
        key = "MqttUser";
    else if (password)
        key = "MqttPassword";
    else if (topic)
        key = support::ascii_iequals(name, "topic") ? "Topic" : "MqttTopic";
    else if (full)
        key = "FullTopic";
    else if (prefix)
        key = "Prefix";
    else if (group)
        key = "GroupTopic";
    else if (port)
        key = "MqttPort";
    else if (keep)
        key = "MqttKeepAlive";
    else if (retry)
        key = "MqttRetry";
    else if (timeout)
        key = "MqttTimeout";
    else if (enable) {
        key = "MqttEnable";
        state = &candidate.enabled;
    } else if (tls) {
        key = "MqttTLS";
        state = &candidate.secure;
    } else
        return CommandStatus::unknown;
    const auto index = request.has_index ? request.index : 1U;
    if ((prefix && (index < 1U || index > 3U)) ||
        (group && (index < 1U || index > MqttSettings::kGroupCount)) ||
        (request.has_index && !prefix && !group))
        return CommandStatus::invalid;
    if (!request.is_query()) {
        bool valid = true;
        auto value = request.payload;
        if (host)
            valid = candidate.set_host(value == "0" ? std::string_view{} : value);
        else if (client)
            valid = candidate.set_client_id(value == "0" ? std::string_view{} : value);
        else if (user)
            valid = candidate.set_username(value == "0" ? std::string_view{} : value);
        else if (password)
            valid = candidate.set_password(value == "0" ? std::string_view{} : value);
        else if (topic)
            valid = candidate.set_topic(value == "1" ? MqttSettings::kDefaultTopicTemplate : value);
        else if (full)
            valid =
                candidate.set_full_topic(value == "1" ? MqttSettings::kDefaultFullTopic : value);
#ifndef NEWMOTA_IMAGE_SAFEBOOT
        else if (button_topic)
            valid = candidate.set_button_topic(value == "0"   ? std::string_view{}
                                               : value == "1" ? current.topic()
                                               : value == "2" ? std::string_view{"0"}
                                                              : value);
#endif
#if USE_SWITCHES
        else if (switch_topic)
            valid = candidate.set_switch_topic(value == "0"   ? std::string_view{}
                                               : value == "1" ? current.topic()
                                               : value == "2" ? std::string_view{"0"}
                                                              : value);
#endif
        else if (prefix) {
            constexpr std::string_view defaults[]{"cmnd", "stat", "tele"};
            valid = candidate.set_prefix(index - 1U, value == "1" ? defaults[index - 1U] : value);
        } else if (group)
            valid =
                candidate.set_group_topic(index - 1U, value == "0"   ? std::string_view{}
                                                      : value == "1" ? std::string_view{"tasmotas"}
                                                                     : value);
        else if (state != nullptr) {
            const auto parsed = parse_command_state(value);
            valid = parsed == CommandState::off || parsed == CommandState::on;
            if (valid)
                *state = parsed == CommandState::on;
            if (candidate.power_retain)
                candidate.only_json = false;
        } else {
            std::uint32_t number = 0U;
            valid = support::parse_unsigned_decimal(value, UINT16_MAX, number);
            if (valid && port)
                valid =
                    candidate.set_port(number == 1U ? 1883U : static_cast<std::uint16_t>(number));
            else if (valid && keep)
                candidate.keep_alive_seconds = static_cast<std::uint16_t>(number);
            else if (valid && retry)
                candidate.retry_seconds = static_cast<std::uint16_t>(number);
            else if (valid && timeout)
                candidate.timeout_seconds = static_cast<std::uint16_t>(number);
        }
        if (!valid)
            return CommandStatus::invalid;
        const auto status = update_status(settings.set_mqtt_settings(candidate));
        if (status != CommandStatus::handled)
            return status;
#ifndef NEWMOTA_IMAGE_SAFEBOOT
        if (button_retain && !candidate.button_retain)
            settings.request_button_retain_clear();
#endif
#if USE_SWITCHES
        if (switch_retain && !candidate.switch_retain)
            settings.request_switch_retain_clear();
#endif
    }
    if (password)
        return write_mqtt_password(output) ? CommandStatus::handled : CommandStatus::failed;
    bool written = false;
    if (host)
        written = write_mqtt_text(output, key, candidate.host());
    else if (client)
        written = write_mqtt_text(output, key, candidate.client_id_template());
    else if (user)
        written = write_mqtt_text(output, key, candidate.username());
    else if (topic)
        written = write_mqtt_text(output, key, candidate.topic());
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    else if (button_topic)
        written = write_mqtt_text(output, key, candidate.button_topic());
#endif
    else if (switch_topic)
        written = write_mqtt_text(output, key, candidate.switch_topic());
    else if (full)
        written = write_mqtt_text(output, key, candidate.full_topic());
    else if (prefix || group) {
        written = output.format("{\"%s%u\":", key, static_cast<unsigned>(index)) &&
                  output.json_string(
                      prefix ? candidate.prefix(index - 1U) : candidate.group_topic(index - 1U)) &&
                  output.write("}");
    } else if (state)
        written = output.format("{\"%s\":\"%s\"}", key, *state ? "ON" : "OFF");
    else
        written = output.format("{\"%s\":%u}", key,
            static_cast<unsigned>(port    ? candidate.port
                                  : keep  ? candidate.keep_alive_seconds
                                  : retry ? candidate.retry_seconds
                                          : candidate.timeout_seconds));
    return written ? CommandStatus::handled : CommandStatus::failed;
}
#endif

/** @brief Handles the shared Tasmota-compatible telemetry cadence command. */
CommandStatus tele_period_command(
    const CommandRequest& request, Settings& settings, CommandOutput& output) {
    if (request.has_index)
        return CommandStatus::invalid;
    if (!request.is_query()) {
        std::uint32_t value = 0U;
        if (!support::parse_unsigned_decimal(
                request.payload, TelemetrySettings::kMaximumPeriodSeconds, value)) {
            return CommandStatus::invalid;
        }
        auto seconds = static_cast<std::uint16_t>(value);
        // Tasmota uses one as the command value for restoring its default.
        if (seconds == 1U) {
            seconds = TelemetrySettings::kDefaultPeriodSeconds;
        }
        const auto status = update_status(settings.set_tele_period_seconds(seconds));
        if (status != CommandStatus::handled)
            return status;
    } else {
        // A query also requests one immediate latest-snapshot publication.
        settings.request_telemetry();
    }
    return output.format(
               "{\"TelePeriod\":%u}", static_cast<unsigned int>(settings.tele_period_seconds()))
               ? CommandStatus::handled
               : CommandStatus::failed;
}

/**
 * @brief Writes boolean SetOption indices in Tasmota-compatible report form.
 */
[[nodiscard]] bool write_set_options(
    CommandOutput& output, Settings& settings, const bool only_set) {
    if (!output.write(only_set ? "\"SetOptionSet\":[" : "\"SetOption\":\"")) {
        return false;
    }
    bool first = true;
    for (std::uint16_t index = 0; index <= Settings::kHighestReportedOption; ++index) {
        if (Settings::is_parameter_option(index)) {
            continue;
        }
        std::uint8_t value = 0;
        (void)settings.option(index, value);
        if (only_set && value == 0) {
            continue;
        }
        if (!output.format("%s%s%u", first ? "" : ",", !only_set && value == 0 ? "!" : "",
                static_cast<unsigned int>(index))) {
            return false;
        }
        first = false;
    }
    return output.write(only_set ? "]" : "\"");
}

/**
 * @brief Writes the SetOption32 through SetOption49 byte array.
 */
[[nodiscard]] bool write_parameters(CommandOutput& output, Settings& settings) {
    if (!output.write("\"SetOption32_49\":[")) {
        return false;
    }
    for (std::uint16_t index = 32; index <= 49; ++index) {
        std::uint8_t value = 0;
        (void)settings.option(index, value);
        if (!output.format("%s%u", index == 32 ? "" : ",", static_cast<unsigned int>(value))) {
            return false;
        }
    }
    return output.write("]");
}

#if USE_BLE
/** @brief Starts or stops BLE for SetOption115. */
void apply_ble(bool on) {
    if (on ? !platform::ble_start() : !platform::ble_stop())
        AddLog(LogLevel::error, on ? "BLE: start failed" : "BLE: stop failed");
    else if (on)
        AddLog(LogLevel::info, "BLE: started");
}
#endif

/**
 * @brief Handles SetOption and SO queries and updates.
 */
CommandStatus set_option_command(
    const CommandRequest& request, Settings& settings, RestartController&, CommandOutput& output) {
    if (!request.has_index) {
        std::uint8_t report = UINT8_MAX;
        if (!request.is_query()) {
            std::uint32_t parsed = 0;
            if (!support::parse_unsigned_decimal(request.payload, 2, parsed)) {
                return CommandStatus::invalid;
            }
            report = static_cast<std::uint8_t>(parsed);
        }
        if (!output.write("{")) {
            return CommandStatus::failed;
        }
        bool okay = true;
        if (report == UINT8_MAX) {
            okay = write_set_options(output, settings, false);
        } else {
            if (report <= 1) {
                okay = write_set_options(output, settings, true);
            }
            if (okay && (report == 0 || report == 2)) {
                if (report == 0) {
                    okay = output.write(",");
                }
                if (okay) {
                    okay = write_parameters(output, settings);
                }
            }
        }
        return okay && output.write("}") ? CommandStatus::handled : CommandStatus::failed;
    }

    std::uint8_t value = 0;
    if (!settings.option(request.index, value)) {
        return CommandStatus::invalid;
    }
    if (request.index == 34U && !request.is_query()) {
        return CommandStatus::invalid;
    }
#if USE_ENERGY_MAX_POWER
    /** Source SO33 accepts1..250; invalid payload queries the retained value. */
    if (request.index == 33 && !request.is_query()) {
        std::uint32_t parsed{};
        if (!support::parse_unsigned_decimal(request.payload, 250, parsed) || !parsed)
            return output.format("{\"SetOption33\":%u}", unsigned(value)) ? CommandStatus::handled
                                                                          : CommandStatus::failed;
    }
#endif
    if (!request.is_query()) {
        if (Settings::is_parameter_option(request.index)) {
            std::uint32_t parsed = 0;
            if (!support::parse_unsigned_decimal(request.payload, UINT8_MAX, parsed)) {
                return CommandStatus::invalid;
            }
            value = static_cast<std::uint8_t>(parsed);
        } else {
            const auto state = parse_command_state(request.payload);
            if (state != CommandState::off && state != CommandState::on) {
                return CommandStatus::invalid;
            }
            value = state == CommandState::on ? 1 : 0;
        }
#if USE_ENERGY
        /** Close the old policy interval before SetOption162 starts affecting new deltas. */
        if (request.index == 162)
            energy::service().poll(platform::monotonic_milliseconds());
#endif
        const auto result = settings.set_option(request.index, value);
        if (result == SettingsUpdateResult::invalid) {
            return CommandStatus::invalid;
        }
        if (result == SettingsUpdateResult::failed) {
            return CommandStatus::failed;
        }
#if USE_SENSORS
        if (request.index == 8U || request.index == 24U || request.index == 139U)
            sensor_service().set_presentation(settings.sensor_presentation());
#endif
#if USE_BLE
        if (request.index == 115U)
            apply_ble(value != 0U);
#endif
    }
    return write_option(output, request.index, value) ? CommandStatus::handled
                                                      : CommandStatus::failed;
}

/** @brief Handles the persistent relay-state save interval. */
CommandStatus save_data_command(const CommandRequest& request, Settings& settings,
    const PowerService& power, CommandOutput& output) {
    if (request.has_index) {
        return CommandStatus::invalid;
    }
    if (!request.is_query()) {
        std::uint32_t seconds = 0U;
        if (!support::parse_unsigned_decimal(request.payload, 3600U, seconds)) {
            return CommandStatus::invalid;
        }
        const auto status = update_status(
            settings.set_save_data(static_cast<std::uint16_t>(seconds), power.state_mask()));
        if (status != CommandStatus::handled) {
            return status;
        }
    }

    const auto seconds = settings.save_data_seconds();
    if (seconds == 0U || seconds == 1U) {
        return output.format("{\"SaveData\":\"%s\"}", seconds == 0U ? "OFF" : "ON")
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    }
    return output.format("{\"SaveData\":\"Every %u seconds\"}", static_cast<unsigned int>(seconds))
               ? CommandStatus::handled
               : CommandStatus::failed;
}

/** @brief Handles Tasmota's six global relay startup policies. */
CommandStatus power_on_state_command(
    const CommandRequest& request, Settings& settings, PowerService& power, CommandOutput& output) {
    if (request.has_index) {
        return CommandStatus::invalid;
    }
    if (!request.is_query()) {
        std::uint32_t value = 0U;
        if (!support::parse_unsigned_decimal(request.payload, 5U, value)) {
            return CommandStatus::invalid;
        }
        const auto status =
            update_status(settings.set_power_on_state(static_cast<std::uint8_t>(value)));
        if (status != CommandStatus::handled) {
            return status;
        }
        if (power.set_always_on(value == 4U) == PowerChangeResult::failed) {
            return CommandStatus::failed;
        }
        const auto now = platform::monotonic_milliseconds();
        for (std::size_t index = 0; index < kMaximumPowerOutputs; ++index) {
            power.update_pulse_time(static_cast<std::uint8_t>(index), now);
        }
    }
    return output.format(
               "{\"PowerOnState\":%u}", static_cast<unsigned int>(settings.power_on_state()))
               ? CommandStatus::handled
               : CommandStatus::failed;
}

/** @brief Streams all configured and remaining PulseTime values. */
[[nodiscard]] bool write_all_pulse_times(CommandOutput& output, const Settings& settings,
    const PowerService& power, const std::uint32_t now) {
    if (!output.write("{\"PulseTime\":{\"Set\":[")) {
        return false;
    }
    for (std::size_t index = 0; index < kMaximumPowerOutputs; ++index) {
        if (!output.format("%s%u", index == 0U ? "" : ",",
                static_cast<unsigned int>(settings.power_settings().pulse_times[index]))) {
            return false;
        }
    }
    if (!output.write("],\"Remaining\":[")) {
        return false;
    }
    for (std::size_t index = 0; index < kMaximumPowerOutputs; ++index) {
        if (!output.format("%s%u", index == 0U ? "" : ",",
                static_cast<unsigned int>(
                    power.remaining_pulse_time(static_cast<std::uint8_t>(index), now)))) {
            return false;
        }
    }
    return output.write("]}}");
}

/** @brief Handles indexed persistent relay pulse durations. */
CommandStatus pulse_time_command(
    const CommandRequest& request, Settings& settings, PowerService& power, CommandOutput& output) {
    const auto now = platform::monotonic_milliseconds();
    if (!request.has_index && request.is_query()) {
        return write_all_pulse_times(output, settings, power, now) ? CommandStatus::handled
                                                                   : CommandStatus::failed;
    }
    const std::uint16_t command_index = request.has_index ? request.index : 1U;
    if (command_index == 0U || command_index > kMaximumPowerOutputs) {
        return CommandStatus::invalid;
    }
    const auto index = static_cast<std::uint8_t>(command_index - 1U);
    if (!request.is_query()) {
        std::uint32_t value = 0U;
        if (!support::parse_unsigned_decimal(request.payload, UINT16_MAX, value)) {
            return CommandStatus::invalid;
        }
        const auto status =
            update_status(settings.set_pulse_time(index, static_cast<std::uint16_t>(value)));
        if (status != CommandStatus::handled) {
            return status;
        }
        power.update_pulse_time(index, now);
    }
    return output.format("{\"PulseTime%u\":{\"Set\":%u,\"Remaining\":%u}}",
               static_cast<unsigned int>(command_index),
               static_cast<unsigned int>(settings.power_settings().pulse_times[index]),
               static_cast<unsigned int>(power.remaining_pulse_time(index, now)))
               ? CommandStatus::handled
               : CommandStatus::failed;
}

/** @brief Handles the persistent relay blink half-period. */
CommandStatus blink_time_command(
    const CommandRequest& request, Settings& settings, PowerService& power, CommandOutput& output) {
    if (request.has_index) {
        return CommandStatus::invalid;
    }
    if (!request.is_query()) {
        std::uint32_t value = 0U;
        if (!support::parse_unsigned_decimal(request.payload, 3600U, value) || value < 2U) {
            return CommandStatus::invalid;
        }
        const auto status =
            update_status(settings.set_blink_time(static_cast<std::uint16_t>(value)));
        if (status != CommandStatus::handled) {
            return status;
        }
        power.update_blink_time(platform::monotonic_milliseconds());
    }
    return output.format("{\"BlinkTime\":%u}",
               static_cast<unsigned int>(settings.power_settings().blink_time))
               ? CommandStatus::handled
               : CommandStatus::failed;
}

/** @brief Handles the persistent finite or continuous blink count. */
CommandStatus blink_count_command(
    const CommandRequest& request, Settings& settings, PowerService& power, CommandOutput& output) {
    if (request.has_index) {
        return CommandStatus::invalid;
    }
    if (!request.is_query()) {
        std::uint32_t value = 0U;
        if (!support::parse_unsigned_decimal(request.payload, 32000U, value)) {
            return CommandStatus::invalid;
        }
        const auto status =
            update_status(settings.set_blink_count(static_cast<std::uint16_t>(value)));
        if (status != CommandStatus::handled) {
            return status;
        }
        power.update_blink_count();
    }
    return output.format("{\"BlinkCount\":%u}",
               static_cast<unsigned int>(settings.power_settings().blink_count))
               ? CommandStatus::handled
               : CommandStatus::failed;
}

/** @brief Parses strict Tasmota-style comma-separated interlock groups. */
[[nodiscard]] bool parse_interlock_groups(std::string_view payload, const PowerService& power,
    std::array<std::uint32_t, kMaximumInterlockGroups>& groups) {
    groups = {};
    payload = support::trim_ascii(payload);
    if (power.relay_count() < 2U || payload.empty()) {
        return false;
    }

    std::uint32_t assigned = 0U;
    std::size_t group_index = 0U;
    std::size_t position = 0U;
    while (position < payload.size()) {
        while (position < payload.size() && support::ascii_is_space(payload[position])) {
            ++position;
        }
        if (position == payload.size() || group_index == groups.size()) {
            return false;
        }

        const auto group_start = position;
        while (position < payload.size() && !support::ascii_is_space(payload[position])) {
            ++position;
        }
        const auto group_text = payload.substr(group_start, position - group_start);
        std::uint32_t group = 0U;
        std::uint8_t members = 0U;
        std::size_t member_start = 0U;
        bool comma = false;
        while (member_start < group_text.size()) {
            const auto separator = group_text.find(',', member_start);
            const auto member = group_text.substr(member_start, separator - member_start);
            std::uint32_t relay = 0U;
            if (member.empty() ||
                !support::parse_unsigned_decimal(member, kMaximumPowerOutputs, relay) ||
                relay == 0U || !power.has_relay(static_cast<std::uint8_t>(relay))) {
                return false;
            }
            const auto mask = 1U << (relay - 1U);
            if ((assigned & mask) != 0U || (group & mask) != 0U) {
                return false;
            }
            group |= mask;
            ++members;
            if (separator == std::string_view::npos) {
                break;
            }
            if (separator + 1U == group_text.size()) {
                return false;
            }
            comma = true;
            member_start = separator + 1U;
        }
        if (!comma || members < 2U) {
            return false;
        }
        groups[group_index++] = group;
        assigned |= group;
    }
    return group_index != 0U;
}

/** @brief Streams interlock state and the effective relay groups. */
[[nodiscard]] bool write_interlock(
    CommandOutput& output, const Settings& settings, const PowerService& power) {
    const auto& configuration = settings.power_settings();
    if (power.relay_count() < 2U) {
        return output.format(
            "{\"Interlock\":\"%s\"}", configuration.interlock_enabled ? "ON" : "OFF");
    }
    if (!output.format("{\"Interlock\":\"%s\",\"Groups\":\"",
            configuration.interlock_enabled ? "ON" : "OFF")) {
        return false;
    }
    bool explicit_groups = false;
    bool first_group = true;
    for (const auto group : configuration.interlock_groups) {
        if (group == 0U) {
            continue;
        }
        explicit_groups = true;
        if (!first_group && !output.write(" ")) {
            return false;
        }
        bool first_relay = true;
        for (std::uint8_t relay = 1U; relay <= kMaximumPowerOutputs; ++relay) {
            if ((group & (1U << (relay - 1U))) == 0U) {
                continue;
            }
            if (!output.format("%s%u", first_relay ? "" : ",", static_cast<unsigned int>(relay))) {
                return false;
            }
            first_relay = false;
        }
        first_group = false;
    }
    if (!explicit_groups) {
        bool first_relay = true;
        for (std::uint8_t relay = 1U; relay <= kMaximumPowerOutputs; ++relay) {
            if (!power.has_relay(relay)) {
                continue;
            }
            if (!output.format("%s%u", first_relay ? "" : ",", static_cast<unsigned int>(relay))) {
                return false;
            }
            first_relay = false;
        }
    }
    return output.write("\"}");
}

/** @brief Handles interlock enablement and group configuration. */
CommandStatus interlock_command(
    const CommandRequest& request, Settings& settings, PowerService& power, CommandOutput& output) {
    if (request.has_index) {
        return CommandStatus::invalid;
    }
    if (!request.is_query()) {
        auto groups = settings.power_settings().interlock_groups;
        bool enabled = settings.power_settings().interlock_enabled;
        if (request.payload.find(',') != std::string_view::npos) {
            if (!parse_interlock_groups(request.payload, power, groups)) {
                return CommandStatus::invalid;
            }
        } else {
            const auto state = parse_command_state(request.payload);
            if (state != CommandState::off && state != CommandState::on) {
                return CommandStatus::invalid;
            }
            enabled = state == CommandState::on;
        }
        const auto status = update_status(settings.set_interlock(enabled, groups));
        if (status != CommandStatus::handled) {
            return status;
        }
        if (power.apply_interlock() == PowerChangeResult::failed) {
            return CommandStatus::failed;
        }
    }
    return write_interlock(output, settings, power) ? CommandStatus::handled
                                                    : CommandStatus::failed;
}

/** @brief Handles one persistent Tasmota log-level command. */
CommandStatus log_level_command(
    const CommandRequest& request, Settings& settings, const bool serial, CommandOutput& output) {
    if (request.has_index) {
        return CommandStatus::invalid;
    }
    if (!request.is_query()) {
        std::uint32_t value = 0U;
        if (!support::parse_unsigned_decimal(request.payload, Settings::kMaximumLogLevel, value)) {
            return CommandStatus::invalid;
        }
        const auto result = serial ? settings.set_serial_log_level(static_cast<std::uint8_t>(value))
                                   : settings.set_web_log_level(static_cast<std::uint8_t>(value));
        const auto status = update_status(result);
        if (status != CommandStatus::handled) {
            return status;
        }
        // The web sink is not present yet, but keep its runtime threshold in
        // sync so adding that sink later will not change command semantics.
        ConfigureLogLevels(settings.serial_log_level(), settings.web_log_level());
    }

    const auto level = serial ? settings.serial_log_level() : settings.web_log_level();
    return output.format(
               "{\"%s\":%u}", serial ? "SerialLog" : "WebLog", static_cast<unsigned int>(level))
               ? CommandStatus::handled
               : CommandStatus::failed;
}

#ifndef NEWMOTA_IMAGE_SAFEBOOT
/** @brief Handles ButtonDebounce without silently accepting unsupported AC detection. */
CommandStatus button_debounce_command(
    const CommandRequest& request, Settings& settings, CommandOutput& output) {
    if (request.has_index)
        return CommandStatus::invalid;
    if (!request.is_query()) {
        std::uint32_t value = 0U;
        if (!support::parse_unsigned_decimal(
                request.payload, Settings::kMaximumButtonDebounce, value) ||
            value < Settings::kMinimumButtonDebounce)
            return CommandStatus::invalid;
        if (value % 10U == 9U) {
            (void)output.write("{\"ButtonDebounce\":\"AC detection unsupported\"}");
            return CommandStatus::invalid;
        }
        const auto status =
            update_status(settings.set_button_debounce(static_cast<std::uint16_t>(value)));
        if (status != CommandStatus::handled)
            return status;
    }
    return output.format("{\"ButtonDebounce\":%u}", unsigned(settings.button_debounce()))
               ? CommandStatus::handled
               : CommandStatus::failed;
}
#endif

#if USE_SWITCHES
/** @brief Handles SwitchDebounce with the ButtonDebounce rules, rejecting AC detection. */
CommandStatus switch_debounce_command(
    const CommandRequest& request, Settings& settings, CommandOutput& output) {
    if (request.has_index)
        return CommandStatus::invalid;
    if (!request.is_query()) {
        std::uint32_t value = 0U;
        if (!support::parse_unsigned_decimal(
                request.payload, Settings::kMaximumButtonDebounce, value) ||
            value < Settings::kMinimumButtonDebounce)
            return CommandStatus::invalid;
        if (value % 10U == 9U) {
            (void)output.write("{\"SwitchDebounce\":\"AC detection unsupported\"}");
            return CommandStatus::invalid;
        }
        const auto status =
            update_status(settings.set_switch_debounce(static_cast<std::uint16_t>(value)));
        if (status != CommandStatus::handled)
            return status;
    }
    return output.format("{\"SwitchDebounce\":%u}", unsigned(settings.switch_debounce()))
               ? CommandStatus::handled
               : CommandStatus::failed;
}

/** @brief Handles SwitchMode<x> 0..16; SwitchMode0 reads or sets all, as in Tasmota. */
CommandStatus switch_mode_command(
    const CommandRequest& request, Settings& settings, CommandOutput& output) {
    const std::size_t index = request.has_index ? request.index : 1U;
    if (index > Settings::kSwitchCount)
        return CommandStatus::invalid;
    if (!request.is_query()) {
        std::uint32_t value = 0U;
        if (!support::parse_unsigned_decimal(
                request.payload, Settings::kSwitchModeCount - 1U, value))
            return CommandStatus::invalid;
        const auto status =
            update_status(settings.set_switch_mode(index, static_cast<std::uint8_t>(value)));
        if (status != CommandStatus::handled)
            return status;
    }
    const auto& modes = settings.switch_modes();
    if (index != 0U)
        return output.format("{\"SwitchMode%u\":%u}", unsigned(index), unsigned(modes[index - 1U]))
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    if (!output.write("{\"SwitchMode\":["))
        return CommandStatus::failed;
    for (std::size_t i = 0U; i < modes.size(); ++i)
        if (!output.format(i == 0U ? "%u" : ",%u", unsigned(modes[i])))
            return CommandStatus::failed;
    return output.write("]}") ? CommandStatus::handled : CommandStatus::failed;
}

/** @brief Handles SwitchText<x>; spaces are removed and a leading quote clears, as in Tasmota.
 * SwitchText without index or value lists all of them.
 */
CommandStatus switch_text_command(
    const CommandRequest& request, Settings& settings, CommandOutput& output) {
    const auto write_text = [&](const std::size_t index) {
        const auto text = settings.switch_text(index);
        return output.format("\"SwitchText%u\":", unsigned(index)) && output.json_string(text);
    };
    if (!request.has_index && request.is_query()) {
        for (std::size_t index = 1U; index <= Settings::kSwitchTextCount; ++index)
            if (!output.write(index == 1U ? "{" : ",") || !write_text(index))
                return CommandStatus::failed;
        return output.write("}") ? CommandStatus::handled : CommandStatus::failed;
    }
    const std::size_t index = request.has_index ? request.index : 1U;
    if (index == 0U || index > Settings::kSwitchTextCount)
        return CommandStatus::invalid;
    if (!request.is_query()) {
        std::array<char, Settings::kSwitchTextCapacity> text{};
        std::size_t length = 0U;
        if (!request.payload.empty() && request.payload.front() != '"') {
            for (const auto character : request.payload) {
                if (character == ' ')
                    continue;
                if (length + 1U >= text.size())
                    return CommandStatus::invalid;
                text[length++] = character;
            }
        }
        const auto status =
            update_status(settings.set_switch_text(index, std::string_view{text.data(), length}));
        if (status != CommandStatus::handled)
            return status;
    }
    return output.write("{") && write_text(index) && output.write("}") ? CommandStatus::handled
                                                                       : CommandStatus::failed;
}
#endif

#if USE_SLEEP // Safeboot never sleeps, so it has no Sleep command
/** @brief Handles Tasmota's Sleep command; the main loop and Wi-Fi apply it on their next pass. */
CommandStatus sleep_command(
    const CommandRequest& request, Settings& settings, CommandOutput& output) {
    if (request.has_index)
        return CommandStatus::invalid;
    if (!request.is_query()) {
        std::uint32_t value = 0U;
        if (!support::parse_unsigned_decimal(request.payload, Settings::kMaximumSleep, value))
            return CommandStatus::invalid;
        const auto status = update_status(settings.set_sleep(static_cast<std::uint8_t>(value)));
        if (status != CommandStatus::handled)
            return status;
    }
    return output.format("{\"Sleep\":{\"%u\":{\"Active\":\"%u\"}}}",
               static_cast<unsigned int>(settings.sleep()),
               static_cast<unsigned int>(settings.active_sleep()))
               ? CommandStatus::handled
               : CommandStatus::failed;
}
#endif

#if USE_WIFI_POWER
/** @brief Handles Tasmota's WifiPower command; Wi-Fi applies it on its next pass. */
CommandStatus wifi_power_command(
    const CommandRequest& request, Settings& settings, CommandOutput& output) {
    if (request.has_index)
        return CommandStatus::invalid;
    if (!request.is_query()) {
        std::uint32_t tenths = 0U;
        if (!support::parse_unsigned_tenths(request.payload, tenths))
            return CommandStatus::invalid;
        const auto status =
            update_status(settings.set_wifi_power(wifi_tx_power::saved_value(tenths)));
        if (status != CommandStatus::handled)
            return status;
    }
    const unsigned int power = wifi_tx_power::reported(settings);
    return output.format("{\"WifiPower\":%u.%u}", power / 10U, power % 10U) ? CommandStatus::handled
                                                                            : CommandStatus::failed;
}
#endif

/** @brief Handles the persisted Tasmota-compatible DeviceName command. */
CommandStatus device_name_command(
    const CommandRequest& request, Settings& settings, CommandOutput& output) {
    if (request.has_index) {
        return CommandStatus::invalid;
    }
    if (!request.is_query()) {
        const auto value = request.payload == "0" || request.payload == "1"
                               ? Settings::kDefaultDeviceName
                               : request.payload;
        const auto status = update_status(settings.set_device_name(value));
        if (status != CommandStatus::handled) {
            return status;
        }
    }
    return output.write("{\"DeviceName\":") && output.json_string(settings.device_name()) &&
                   output.write("}")
               ? CommandStatus::handled
               : CommandStatus::failed;
}

#if USE_OTA_PULL
/** @brief Handles the persistent Tasmota-compatible OtaUrl setting. */
CommandStatus ota_url_command(
    const CommandRequest& request, Settings& settings, CommandOutput& output) {
    if (request.has_index) {
        return CommandStatus::invalid;
    }
    if (!request.is_query()) {
        const auto value = request.payload == "0" ? std::string_view{} : request.payload;
        const auto status = update_status(settings.set_ota_url(value));
        if (status != CommandStatus::handled) {
            return status;
        }
    }
    return output.write("{\"OtaUrl\":") && output.json_string(settings.ota_url()) &&
                   output.write("}")
               ? CommandStatus::handled
               : CommandStatus::failed;
}
#endif

} // namespace

/**
 * @brief Dispatches settings and Wi-Fi credential commands.
 */
CommandStatus dispatch_settings(const CommandRequest& request, Settings& settings,
    RestartController& restart, PowerService& power, CommandOutput& output) {
#if USE_ENERGY_PRECISION
    const auto precision = energy::dispatch_precision(request, settings, output);
    if (precision != CommandStatus::unknown)
        return precision;
#endif
#if USE_SENSORS
    if (support::ascii_iequals(request.name, "sensordriver"))
        return sensor_driver_command(request, settings, output);
    if (support::ascii_iequals(request.name, "tempres"))
        return sensor_output_command(
            request, settings, output, SensorOutputSetting::temperature_resolution);
    if (support::ascii_iequals(request.name, "humres"))
        return sensor_output_command(
            request, settings, output, SensorOutputSetting::humidity_resolution);
    if (support::ascii_iequals(request.name, "pressres"))
        return sensor_output_command(
            request, settings, output, SensorOutputSetting::pressure_resolution);
    if (support::ascii_iequals(request.name, "tempoffset"))
        return sensor_output_command(
            request, settings, output, SensorOutputSetting::temperature_offset);
    if (support::ascii_iequals(request.name, "humoffset"))
        return sensor_output_command(
            request, settings, output, SensorOutputSetting::humidity_offset);
#endif
#if USE_MQTT
    const auto mqtt_status = mqtt_command(request, settings, output);
    if (mqtt_status != CommandStatus::unknown) {
        return mqtt_status;
    }
#endif
    if (support::ascii_iequals(request.name, "setoption") ||
        support::ascii_iequals(request.name, "so")) {
        return set_option_command(request, settings, restart, output);
    }
    if (support::ascii_iequals(request.name, "teleperiod")) {
        return tele_period_command(request, settings, output);
    }
    if (support::ascii_iequals(request.name, "ap")) {
        return ap_command(request, settings, output);
    }
    if (support::ascii_iequals(request.name, "ssid")) {
        return ssid_command(request, settings, output);
    }
    if (support::ascii_iequals(request.name, "password")) {
        return password_command(request, settings, output);
    }
    if (support::ascii_iequals(request.name, "webpassword")) {
        if (request.has_index)
            return CommandStatus::invalid;
        if (!request.is_query()) {
            // Initial provisioning and replacement require the physical console.
            if (request.source != CommandSource::serial)
                return CommandStatus::invalid;
            const auto status = update_status(settings.set_web_password(
                request.payload == "0" ? std::string_view{} : request.payload));
            if (status != CommandStatus::handled)
                return status;
        }
        return output.write("{\"WebPassword\":\"****\"}") ? CommandStatus::handled
                                                          : CommandStatus::failed;
    }
    if (support::ascii_iequals(request.name, "devicename")) {
        return device_name_command(request, settings, output);
    }
#if USE_OTA_PULL
    if (support::ascii_iequals(request.name, "otaurl")) {
        return ota_url_command(request, settings, output);
    }
#endif
    if (support::ascii_iequals(request.name, "savedata")) {
        return save_data_command(request, settings, power, output);
    }
    if (support::ascii_iequals(request.name, "poweronstate")) {
        return power_on_state_command(request, settings, power, output);
    }
    if (support::ascii_iequals(request.name, "pulsetime")) {
        return pulse_time_command(request, settings, power, output);
    }
    if (support::ascii_iequals(request.name, "blinktime")) {
        return blink_time_command(request, settings, power, output);
    }
    if (support::ascii_iequals(request.name, "blinkcount")) {
        return blink_count_command(request, settings, power, output);
    }
    if (support::ascii_iequals(request.name, "interlock")) {
        return interlock_command(request, settings, power, output);
    }
    if (support::ascii_iequals(request.name, "seriallog")) {
        return log_level_command(request, settings, true, output);
    }
    if (support::ascii_iequals(request.name, "weblog")) {
        return log_level_command(request, settings, false, output);
    }
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    if (support::ascii_iequals(request.name, "buttondebounce"))
        return button_debounce_command(request, settings, output);
#endif
#if USE_SWITCHES
    if (support::ascii_iequals(request.name, "switchdebounce"))
        return switch_debounce_command(request, settings, output);
    if (support::ascii_iequals(request.name, "switchmode"))
        return switch_mode_command(request, settings, output);
    if (support::ascii_iequals(request.name, "switchtext"))
        return switch_text_command(request, settings, output);
#endif
#if USE_SLEEP
    if (support::ascii_iequals(request.name, "sleep"))
        return sleep_command(request, settings, output);
#endif
#if USE_WIFI_POWER
    if (support::ascii_iequals(request.name, "wifipower"))
        return wifi_power_command(request, settings, output);
#endif
    return CommandStatus::unknown;
}

} // namespace newmota::commands
