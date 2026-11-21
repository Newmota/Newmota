/*
  status_commands.cpp - status commands for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Status9 reference: Tasmota support_command.ino / xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later.
*/

/** @file status_commands.cpp
 * @brief Builds Tasmota-shaped status responses from current application state.
 */

#include "services/status/status_commands.h"

#include "newmota_features.h"
#if USE_WIFI
#include "newmota/platform/network.h"
#endif
#include "newmota/platform/platform.h"
#include "core/system/boot_counter.h"
#include "core/system/boot_guard.h"
#include "core/system/uptime.h"
#include "services/settings/settings.h"
#if USE_ENERGY_STATUS9
#include "services/energy/energy_service.h"
#endif
#include "services/power/power_service.h"
#include "services/power/power_telemetry.h"
#if USE_TASMOTA_STATE && USE_MQTT
#include "services/mqtt/mqtt_wire.h"
#endif
#if USE_TIMERS
#include "services/timers/timer_service.h"
#endif
#if USE_WIFI_POWER
#include "services/network/wifi_tx_power.h"
#endif
#if USE_ETHERNET
#include "services/network/ethernet_service.h"
#endif
#if USE_TIME
#include "services/time/time_service.h"
#endif
#if USE_NTP_SERVER
#include "newmota/platform/ntp_server.h"
#endif
#include "support/number.h"
#include "support/text.h"
#if USE_GPS
#include "drivers/gps/gps_service.h"
#endif
#if USE_RTC_CHIPS
#include "drivers/rtc/rtc_service.h"
#endif

#if USE_SENSORS || USE_ENERGY || USE_SWITCHES
#include "services/telemetry/measurement_document.h"
#endif

#include <array>
#include <cstddef>
#include <cstdint>

namespace newmota::commands {
namespace {

#if USE_TASMOTA_STATE
StateWriter state_writer{};
#endif
#if USE_TASMOTA_STATE && USE_MQTT
MqttStatusReader mqtt_status_reader{};

/** One Status 0 publication over MQTT: its remaining sections, one bit each, and its routing. */
struct StatusSplit final {
    std::uint16_t sections{};
    bool fallback{};
};

/** The split being published, and the newest request that arrived during it. */
StatusSplit active_split{};
StatusSplit repeat_split{};
/** Requested by the current dispatch; the processor decides whether to deliver it. */
StatusSplit requested_split{};
bool request_fallback{};
#endif
#if USE_TASMOTA_STATE
std::uint8_t sensor_section = 10U;
#endif

constexpr std::uint8_t kMaximumStatus = 13;
constexpr std::array<std::uint8_t, 13> kAllSections{0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13};

/**
 * @brief Maps a numeric Status section to its JSON object name.
 */
[[nodiscard]] const char* section_name(const std::uint8_t section) {
    switch (section) {
    case 0:
        return "Status";
    case 1:
        return "StatusPRM";
    case 2:
        return "StatusFWR";
    case 3:
        return "StatusLOG";
    case 4:
        return "StatusMEM";
    case 5:
        return "StatusNET";
    case 7:
        return "StatusTIM";
    case 8:
    case 10:
        return "StatusSNS";
    case 11:
        return "StatusSTS";
    default:
        return nullptr;
    }
}

/**
 * @brief Writes an empty placeholder for a recognized unimplemented section.
 */
[[nodiscard]] bool write_empty_section(CommandOutput& output, const std::uint8_t section) {
    const char* const name = section_name(section);
    return name != nullptr && output.format("\"%s\":{}", name);
}

/**
 * @brief Reports whether this device has anything to say for a section. Like Tasmota, a section
 * without one is an error when requested and left out of the aggregate.
 */
[[nodiscard]] bool section_available(const Settings& settings, const std::uint8_t section) {
    switch (section) {
    case 6:
#if USE_TASMOTA_STATE && USE_MQTT
        return settings.mqtt_settings().enabled;
#else
        (void)settings;
        return false;
#endif
    case 9:
#if USE_ENERGY_STATUS9
        return energy::service().has_margin_status();
#else
        return false;
#endif
    case 12:
    case 13:
        return false;
    default:
        return true;
    }
}

#if USE_TIME
/** @brief Writes one ISO-8601 time value from Unix seconds. */
[[nodiscard]] bool write_time_field(
    CommandOutput& output, const std::uint32_t seconds, const bool utc) {
    const auto value = TimeService::civil_time(seconds);
    return output.format("\"%04u-%02u-%02uT%02u:%02u:%02u%s\"", static_cast<unsigned>(value.year),
        static_cast<unsigned>(value.month), static_cast<unsigned>(value.day),
        static_cast<unsigned>(value.hour), static_cast<unsigned>(value.minute),
        static_cast<unsigned>(value.second), utc ? "Z" : "");
}
#endif

#if USE_TASMOTA_STATE
/** @brief Writes logical Power as Tasmota's binary string, with the first endpoint rightmost. */
[[nodiscard]] bool write_power_mask(CommandOutput& output, const PowerService& power) {
    std::array<char, kMaximumPowerOutputs> bits{};
    std::size_t count = 0U;
    for (auto instance = static_cast<std::uint8_t>(kMaximumPowerOutputs); instance != 0U;
        --instance) {
        bool on = false;
        const bool configured = power.state(instance, on);
        if (count != 0U || configured)
            bits[count++] = on ? '1' : '0';
    }
    if (count == 0U)
        bits[count++] = '0';
    return output.write(",\"Power\":\"") && output.write({bits.data(), count}) &&
           output.write("\"");
}
#endif

/** @brief Writes the root Status identity and power policy in Tasmota order. */
[[nodiscard]] bool write_status_section(
    CommandOutput& output, const Settings& settings, [[maybe_unused]] const PowerService& power) {
    if (!output.format("\"Status\":{\"Module\":%u,\"DeviceName\":",
            static_cast<unsigned int>(settings.hardware_configuration().module)) ||
        !output.json_string(settings.device_name())) {
        return false;
    }
#if USE_TASMOTA_STATE
#if USE_MQTT
    const auto& mqtt = settings.mqtt_settings();
    const auto live = mqtt_status_reader != nullptr ? mqtt_status_reader() : MqttStatus{};
    if (!output.write(",\"Topic\":") ||
        !output.json_string(live.topic.empty() ? mqtt.topic() : live.topic)) {
        return false;
    }
#endif
    std::uint8_t save_state = 0U;
    (void)settings.option(0U, save_state);
    if (!write_power_mask(output, power) ||
        !output.format(
            ",\"PowerOnState\":%u", static_cast<unsigned int>(settings.power_on_state()))) {
        return false;
    }
#if USE_STATUS_LEDS
    if (!output.format(",\"LedState\":%u,\"LedMask\":\"%04X\"",
            static_cast<unsigned int>(settings.power_settings().led_state),
            static_cast<unsigned int>(settings.power_settings().led_mask))) {
        return false;
    }
#endif
    if (!output.format(",\"SaveData\":%u,\"SaveState\":%u",
            static_cast<unsigned int>(settings.save_data_seconds()),
            static_cast<unsigned int>(save_state))) {
        return false;
    }
#if USE_MQTT
    if (!output.format(
            ",\"PowerRetain\":%u,\"InfoRetain\":%u,\"StateRetain\":%u,\"StatusRetain\":%u",
            mqtt.power_retain ? 1U : 0U, mqtt.info_retain ? 1U : 0U, mqtt.state_retain ? 1U : 0U,
            mqtt.status_retain ? 1U : 0U)) {
        return false;
    }
#endif
#endif
    return output.write("}");
}

/**
 * @brief Writes SetOption32 through SetOption49 as a packed hexadecimal string.
 */
[[nodiscard]] bool write_parameters(CommandOutput& output, const Settings& settings) {
    if (!output.write("\"")) {
        return false;
    }
    for (std::uint16_t index = 32; index <= 49; ++index) {
        std::uint8_t value = 0;
        (void)settings.option(index, value);
        if (!output.format("%02X", static_cast<unsigned int>(value))) {
            return false;
        }
    }
    return output.write("\"");
}

/**
 * @brief Writes all boolean SetOption words plus the parameter field.
 */
[[nodiscard]] bool write_set_options(CommandOutput& output, const Settings& settings) {
    std::array<std::uint32_t, 5> words{};
    for (std::uint16_t index = 0; index < Settings::kOptionCount; ++index) {
        if (Settings::is_parameter_option(index)) {
            continue;
        }
        std::uint8_t value = 0;
        (void)settings.option(index, value);
        if (value == 0) {
            continue;
        }
        const auto compact_index = static_cast<std::uint16_t>(index < 32 ? index : index - 18);
        words[compact_index / 32] |= 1U << (compact_index % 32);
    }

    if (!output.format("\"%08lX\",", static_cast<unsigned long>(words[0])) ||
        !write_parameters(output, settings)) {
        return false;
    }
    for (std::size_t index = 1; index < words.size(); ++index) {
        if (!output.format(",\"%08lX\"", static_cast<unsigned long>(words[index]))) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Writes the available StatusPRM console, update, uptime, and boot fields.
 */
[[nodiscard]] bool write_parameter_section(CommandOutput& output, const Settings& settings,
    [[maybe_unused]] const TimeService* const time, const BootStatus* const boot) {
    const std::uint32_t uptime = uptime_seconds();
    const std::uint32_t days = uptime / 86400U;
    const std::uint32_t hours = (uptime / 3600U) % 24U;
    const std::uint32_t minutes = (uptime / 60U) % 60U;
    const std::uint32_t seconds = uptime % 60U;
    const std::uint32_t baudrate = platform::console_baudrate();
    if (!output.write("\"StatusPRM\":{")) {
        return false;
    }
    if (baudrate != 0U &&
        !output.format("\"Baudrate\":%lu,", static_cast<unsigned long>(baudrate))) {
        return false;
    }
#if USE_TASMOTA_STATE
#if USE_MQTT
    if (!output.write("\"GroupTopic\":") ||
        !output.json_string(settings.mqtt_settings().group_topic(0U)) || !output.write(",")) {
        return false;
    }
#endif
    if (!output.write("\"OtaUrl\":") || !output.json_string(settings.ota_url()) ||
        !output.write(",")) {
        return false;
    }
    const char* const restart_reason = platform::restart_reason_name(platform::restart_reason());
    if (restart_reason != nullptr && !output.format("\"RestartReason\":\"%s\",", restart_reason)) {
        return false;
    }
#endif
    if (!output.format("\"Uptime\":\"%luT%02lu:%02lu:%02lu\"", static_cast<unsigned long>(days),
            static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes),
            static_cast<unsigned long>(seconds))) {
        return false;
    }
#if USE_TASMOTA_STATE && USE_TIME
    if (time != nullptr) {
        const auto clock = time->snapshot(platform::monotonic_milliseconds());
        if (clock.valid && (!output.write(",\"StartupUTC\":") ||
                               !write_time_field(output, clock.utc_seconds - uptime, false))) {
            return false;
        }
    }
#endif
#if USE_SLEEP
    if (!output.format(",\"Sleep\":%u", static_cast<unsigned int>(settings.sleep()))) {
        return false;
    }
#endif
    if (boot != nullptr && boot->available &&
        (!output.write(",") ||
            !output.format("\"BootCount\":%lu", static_cast<unsigned long>(boot->count)))) {
        return false;
    }
#if USE_TASMOTA_STATE
    if (!output.format(",\"SaveCount\":%lu", static_cast<unsigned long>(settings.generation()))) {
        return false;
    }
#endif
    if (boot_guard().reduced() && !output.format(",\"BootLoop\":%u",
                                      static_cast<unsigned int>(boot_guard().restart().count))) {
        return false;
    }
    if (settings.restart_required() && (!output.write(",\"RestartRequired\":true"))) {
        return false;
    }
    return output.write("}");
}

/**
 * @brief Writes firmware, build, platform, SDK, and hardware identity as
 * StatusFWR.
 */
[[nodiscard]] bool write_firmware_section(CommandOutput& output) {
    const auto& info = platform::info();
    return output.write("\"StatusFWR\":{\"Version\":") &&
           output.json_string(info.framework_version()) && output.write(",\"BuildDateTime\":") &&
           output.json_string(info.build_datetime()) && output.write(",\"Platform\":") &&
           output.json_string(info.sdk_name()) && output.write(",\"SDK\":") &&
           output.json_string(info.sdk_version()) &&
#if USE_TASMOTA_STATE
           (platform::hardware_info().cpu_frequency_mhz == 0U ||
               output.format(",\"CpuFrequency\":%lu",
                   static_cast<unsigned long>(platform::hardware_info().cpu_frequency_mhz))) &&
#endif
           output.write(",\"Hardware\":") && output.json_string(info.chip()) && output.write("}");
}

/**
 * @brief Writes configured SSIDs and SetOption words as StatusLOG.
 */
[[nodiscard]] bool write_logging_section(CommandOutput& output, const Settings& settings) {
    if (!output.format("\"StatusLOG\":{\"SerialLog\":%u,\"WebLog\":%u,\"SSId\":[",
            static_cast<unsigned int>(settings.serial_log_level()),
            static_cast<unsigned int>(settings.web_log_level())) ||
        !output.json_string(settings.ssid(0)) || !output.write(",") ||
        !output.json_string(settings.ssid(1)) ||
        !output.format("],\"TelePeriod\":%u,\"SetOption\":[",
            static_cast<unsigned int>(settings.tele_period_seconds())) ||
        !write_set_options(output, settings)) {
        return false;
    }
    return output.write("]}");
}

enum class MemoryUnit : std::uint8_t {
    bytes,
    kibibytes,
};

/** @brief Separates one StatusMEM field from the available fields before it. */
[[nodiscard]] bool write_memory_separator(CommandOutput& output, bool& first) {
    if (!first && !output.write(",")) {
        return false;
    }
    first = false;
    return true;
}

/**
 * @brief Appends one available byte measurement in its StatusMEM unit.
 */
[[nodiscard]] bool write_memory_value(CommandOutput& output, bool& first, const char* name,
    const std::uint32_t bytes, const MemoryUnit unit) {
    if (!write_memory_separator(output, first)) {
        return false;
    }
    const std::uint32_t value = unit == MemoryUnit::kibibytes ? bytes / 1024U : bytes;
    return output.format("\"%s\":%lu", name, static_cast<unsigned long>(value));
}

/**
 * @brief Writes only the memory measurements available on this platform.
 */
[[nodiscard]] bool write_memory_section(CommandOutput& output) {
    const auto memory = platform::memory_info();
    if (!output.write("\"StatusMEM\":{")) {
        return false;
    }

    bool first = true;
    const bool program_available =
        memory.program_available && memory.program_size_bytes <= memory.program_capacity_bytes;
    if (program_available &&
        (!write_memory_value(
             output, first, "ProgramSize", memory.program_size_bytes, MemoryUnit::kibibytes) ||
            !write_memory_value(output, first, "Free",
                memory.program_capacity_bytes - memory.program_size_bytes, MemoryUnit::bytes))) {
        return false;
    }
    if (memory.heap_available &&
        !write_memory_value(output, first, "Heap", memory.heap_free_bytes, MemoryUnit::bytes)) {
        return false;
    }
    if (memory.stack_available && !write_memory_value(output, first, "StackLowMark",
                                      memory.stack_low_watermark_bytes, MemoryUnit::bytes)) {
        return false;
    }
#ifdef NEWMOTA_PSRAM_MODE
    if (!write_memory_separator(output, first) ||
        !output.write("\"PsrMode\":\"" NEWMOTA_PSRAM_MODE "\"")) {
        return false;
    }
#endif
    if (memory.psram_available && (!write_memory_value(output, first, "PsrMax",
                                       memory.psram_total_bytes, MemoryUnit::kibibytes) ||
                                      !write_memory_value(output, first, "PsrFree",
                                          memory.psram_free_bytes, MemoryUnit::bytes))) {
        return false;
    }
    if (program_available && !write_memory_value(output, first, "ProgramFlashSize",
                                 memory.program_capacity_bytes, MemoryUnit::kibibytes)) {
        return false;
    }
    if (memory.flash_available && !write_memory_value(output, first, "FlashSize",
                                      memory.flash_size_bytes, MemoryUnit::kibibytes)) {
        return false;
    }
#if USE_TASMOTA_STATE
    const auto hardware = platform::hardware_info();
    if (hardware.flash_chip_id != 0U &&
        (!write_memory_separator(output, first) ||
            !output.format(
                "\"FlashChipId\":\"%06lX\"", static_cast<unsigned long>(hardware.flash_chip_id)))) {
        return false;
    }
    if (hardware.flash_frequency_mhz != 0U &&
        !write_memory_value(
            output, first, "FlashFrequency", hardware.flash_frequency_mhz, MemoryUnit::bytes)) {
        return false;
    }
    if (hardware.flash_mode != nullptr &&
        (!write_memory_separator(output, first) ||
            !output.format("\"FlashMode\":\"%s\"", hardware.flash_mode))) {
        return false;
    }
#endif
    return output.write("}");
}

#if USE_ETHERNET
/** @brief Tasmota's Ethernet block inside StatusNET while the interface runs. */
[[nodiscard]] bool write_ethernet_fields(CommandOutput& output) {
    platform::NetworkInfo info{};
    if (!ethernet::info(info))
        return true;
    return output.write(",\"Ethernet\":{\"Hostname\":") && output.json_string(info.hostname) &&
           output.write(",\"IPAddress\":") && output.json_string(info.address) &&
           output.write(",\"Gateway\":") && output.json_string(info.gateway) &&
           output.write(",\"Subnetmask\":") && output.json_string(info.subnet) &&
           output.write(",\"DNSServer1\":") && output.json_string(info.dns1) &&
           output.write(",\"DNSServer2\":") && output.json_string(info.dns2) &&
           output.write(",\"Mac\":") && output.json_string(info.mac) && output.write("}");
}
#endif

/**
 * @brief Writes Tasmota-compatible station addressing as StatusNET, plus the link's Band.
 */
[[nodiscard]] bool write_network_section(
    CommandOutput& output, [[maybe_unused]] const Settings& settings) {
#if USE_WIFI
    const auto info = platform::network_info();
#if USE_WIFI_POWER
    const unsigned int tx_power = wifi_tx_power::reported(settings);
#endif
    platform::WifiLink link{};
    const char* const band =
        platform::wifi_link(link) ? platform::wifi_band(link.channel) : nullptr;
    return output.write("\"StatusNET\":{\"Hostname\":") && output.json_string(info.hostname) &&
           output.write(",\"IPAddress\":") && output.json_string(info.address) &&
           output.write(",\"Gateway\":") && output.json_string(info.gateway) &&
           output.write(",\"Subnetmask\":") && output.json_string(info.subnet) &&
           output.write(",\"DNSServer1\":") && output.json_string(info.dns1) &&
           output.write(",\"DNSServer2\":") && output.json_string(info.dns2) &&
           output.write(",\"Mac\":") && output.json_string(info.mac) &&
#if USE_ETHERNET
           write_ethernet_fields(output) &&
#endif
           (band == nullptr || output.format(",\"Band\":\"%s\"", band)) &&
#if USE_WIFI_POWER
           output.format(",\"WifiPower\":%u.%u", tx_power / 10U, tx_power % 10U) &&
#endif
           output.write("}");
#else
    return output.write("\"StatusNET\":{}");
#endif
}

/** @brief Writes the sleep and relay fields of StatusSTS in Tasmota order. */
[[nodiscard]] bool write_state_section(CommandOutput& output, const PowerService& power) {
#if !USE_SLEEP
    return output.write("\"StatusSTS\":{") && write_power_state_fields(output, power) &&
           output.write("}");
#else
    return output.write("\"StatusSTS\":{") && write_sleep_state_fields(output) &&
           write_power_state_fields(output, power, false) && output.write("}");
#endif
}

#if USE_TASMOTA_STATE && USE_MQTT
/** @brief Writes broker settings and live client state as StatusMQT, never the password. */
[[nodiscard]] bool write_mqtt_section(CommandOutput& output, const Settings& settings) {
    const auto& mqtt = settings.mqtt_settings();
    const auto live = mqtt_status_reader != nullptr ? mqtt_status_reader() : MqttStatus{};
    return output.write("\"StatusMQT\":{\"MqttHost\":") && output.json_string(mqtt.host()) &&
           output.format(",\"MqttPort\":%u,\"MqttClientMask\":", unsigned(mqtt.port)) &&
           output.json_string(mqtt.client_id_template()) && output.write(",\"MqttClient\":") &&
           output.json_string(live.client) && output.write(",\"MqttUser\":") &&
           output.json_string(mqtt.username()) &&
           output.format(",\"MqttCount\":%lu,\"MqttTLS\":%u,\"MAX_PACKET_SIZE\":%u,"
                         "\"KEEPALIVE\":%u,\"SOCKET_TIMEOUT\":%u}",
               static_cast<unsigned long>(live.connections), mqtt.secure ? 1U : 0U,
               static_cast<unsigned int>(MqttFrameReader::kMaximumPacketSize),
               unsigned(mqtt.keep_alive_seconds), unsigned(mqtt.timeout_seconds));
}
#endif

#if USE_TIME
#if USE_TIMERS
/** @brief Writes today's local solar event as HH:MM; a polar date without one is omitted. */
[[nodiscard]] bool write_solar_field(CommandOutput& output, const Settings& settings,
    const TimeService& time, const std::uint32_t local, const bool sunset) {
    std::uint32_t utc = 0U;
    if (!TimerService::solar_utc(local / 86400U * 86400U, sunset, settings.timer_settings(), utc))
        return true;
    const auto seconds = time.to_local(utc) % 86400U;
    return output.format(",\"%s\":\"%02lu:%02lu\"", sunset ? "Sunset" : "Sunrise",
        static_cast<unsigned long>(seconds / 3600U),
        static_cast<unsigned long>(seconds % 3600U / 60U));
}
#endif

/** @brief Writes Tasmota-compatible UTC, local, DST, and timezone state. */
[[nodiscard]] bool write_time_section(
    CommandOutput& output, const Settings& settings, const TimeService& time) {
    const auto state = time.snapshot(platform::monotonic_milliseconds());
    if (!output.write("\"StatusTIM\":{\"UTC\":") ||
        !write_time_field(output, state.utc_seconds, true) || !output.write(",\"Local\":") ||
        !write_time_field(output, state.local_seconds, false) || !output.write(",\"StartDST\":") ||
        !write_time_field(output, state.daylight_start_seconds, false) ||
        !output.write(",\"EndDST\":") ||
        !write_time_field(output, state.standard_start_seconds, false) ||
        !output.write(",\"Timezone\":")) {
        return false;
    }
    if (settings.timezone_automatic()) {
        if (!output.write("99"))
            return false;
    } else {
        const auto offset = settings.timezone_offset_minutes();
        const auto magnitude =
            static_cast<unsigned>(offset < 0 ? -static_cast<std::int32_t>(offset) : offset);
        if (!output.format(
                "\"%c%02u:%02u\"", offset < 0 ? '-' : '+', magnitude / 60U, magnitude % 60U))
            return false;
    }
#if USE_TIMERS
    if (state.valid && (!write_solar_field(output, settings, time, state.local_seconds, false) ||
                           !write_solar_field(output, settings, time, state.local_seconds, true)))
        return false;
#endif
    if (!output.format(",\"Source\":\"%s\",\"Valid\":%s", time_source_name(state.source),
            state.valid ? "true" : "false"))
        return false;
#if USE_RTC_CHIPS
    const auto chip = rtc::service().snapshot();
    if (!output.format(
            ",\"RTC\":{\"Chip\":\"%s\",\"Bus\":%u,\"State\":\"%s\",\"Valid\":%s,\"Write\":\"%s\"}",
            chip.chip, static_cast<unsigned>(chip.bus), rtc::state_name(chip.state),
            chip.valid ? "true" : "false", rtc::write_result_name(chip.write)))
        return false;
#endif
#if USE_GPS
    const auto gps = gps::service().snapshot(platform::monotonic_milliseconds());
    if (!output.format(",\"GPS\":{\"State\":\"%s\",\"Baud\":%lu,\"Time\":{\"Valid\":%s,\"AgeMs\":%"
                       "lu,\"AccuracyNs\":%lu}",
            gps.state, static_cast<unsigned long>(gps.baud), gps.time_valid ? "true" : "false",
            static_cast<unsigned long>(gps.time_age),
            static_cast<unsigned long>(gps.time_accuracy_ns)) ||
        !output.format(",\"Fix\":{\"Valid\":%s,\"Type\":%u,\"ITOWms\":%lu}",
            gps.fix_valid ? "true" : "false", static_cast<unsigned>(gps.fix.type),
            static_cast<unsigned long>(gps.fix.itow)) ||
        !output.format(",\"Position\":{\"Valid\":%s,\"AgeMs\":%lu,\"LonE7\":%ld,\"LatE7\":%ld",
            gps.position_valid ? "true" : "false", static_cast<unsigned long>(gps.position_age),
            static_cast<long>(gps.position.longitude), static_cast<long>(gps.position.latitude)) ||
        !output.format(",\"AltMm\":%ld,\"HAccMm\":%lu,\"VAccMm\":%lu}",
            static_cast<long>(gps.position.altitude_msl),
            static_cast<unsigned long>(gps.position.horizontal_accuracy),
            static_cast<unsigned long>(gps.position.vertical_accuracy)) ||
        !output.format(
            ",\"Velocity\":{\"Valid\":%s,\"AgeMs\":%lu,\"SpeedCmS\":%lu,\"HeadingE5\":%ld}",
            gps.velocity_valid ? "true" : "false", static_cast<unsigned long>(gps.velocity_age),
            static_cast<unsigned long>(gps.velocity.ground_speed),
            static_cast<long>(gps.velocity.heading)) ||
        !output.format(
            ",\"Errors\":{\"Checksum\":%lu,\"Length\":%lu,\"Timeout\":%lu,\"Rejected\":%lu",
            static_cast<unsigned long>(gps.framing.checksum),
            static_cast<unsigned long>(gps.framing.length),
            static_cast<unsigned long>(gps.framing.timeout),
            static_cast<unsigned long>(gps.rejected)) ||
        !output.format(",\"Retries\":%lu,\"NAKs\":%lu,\"Dropped\":%lu,\"UART\":%lu}}",
            static_cast<unsigned long>(gps.retries), static_cast<unsigned long>(gps.naks),
            static_cast<unsigned long>(gps.dropped), static_cast<unsigned long>(gps.uart_errors)))
        return false;
#endif
#if USE_NTP_SERVER
    const auto now = platform::monotonic_milliseconds();
    const auto server = platform::ntp_server_snapshot();
    const auto quality = platform::ntp_server_quality(time.ntp_server_clock(now), now);
    if (!output.format(",\"NTPServer\":{\"State\":\"%s\",\"Quality\":\"%s\",\"Age\":%lu",
            server.failed      ? "failed"
            : server.listening ? "listening"
                               : "waiting",
            platform::ntp_server_quality_name(quality),
            static_cast<unsigned long>(time.sample_age_seconds(now))) ||
        !output.format(",\"Req\":%lu,\"Sent\":%lu,\"Drop\":%lu}",
            static_cast<unsigned long>(server.requests), static_cast<unsigned long>(server.replies),
            static_cast<unsigned long>(server.dropped)))
        return false;
#endif
    return output.write("}");
}
#endif

/**
 * @brief Writes one selected Status section without the outer JSON object.
 */
[[nodiscard]] bool write_section(CommandOutput& output, const Settings& settings,
    const PowerService& power, const TimeService* const time, const BootStatus* const boot,
    const std::uint8_t section) {
#if !USE_TIME
    (void)time;
#endif
    switch (section) {
    case 0:
        return write_status_section(output, settings, power);
    case 1:
        return write_parameter_section(output, settings, time, boot);
    case 2:
        return write_firmware_section(output);
    case 3:
        return write_logging_section(output, settings);
    case 4:
        return write_memory_section(output);
    case 5:
        return write_network_section(output, settings);
#if USE_TASMOTA_STATE && USE_MQTT
    case 6:
        return write_mqtt_section(output, settings);
#endif
    case 7:
#if USE_TIME
        return time != nullptr && write_time_section(output, settings, *time);
#else
        return write_empty_section(output, section);
#endif
    case 8:
    case 10:
#if USE_SENSORS || USE_ENERGY || USE_SWITCHES
        return output.write("\"StatusSNS\":{") && telemetry::write_measurement_fields(output) &&
               output.write("}");
#else
        return write_empty_section(output, section);
#endif
    case 9:
#if USE_ENERGY_STATUS9
        return energy::service().write_margin_status(output);
#else
        return false;
#endif
    case 11:
#if USE_TASMOTA_STATE
        if (state_writer != nullptr)
            return output.write("\"StatusSTS\":{") && state_writer(output) && output.write("}");
#endif
        return write_state_section(output, power);
    default:
        return write_empty_section(output, section);
    }
}

/**
 * @brief Writes one complete Status response object.
 */
[[nodiscard]] bool write_one(CommandOutput& output, const Settings& settings,
    const PowerService& power, const TimeService* const time, const BootStatus* const boot,
    const std::uint8_t section) {
    return output.write("{") && write_section(output, settings, power, time, boot, section) &&
           output.write("}");
}

/**
 * @brief Writes every standard Status section in Tasmota ordering.
 */
[[nodiscard]] bool write_all(CommandOutput& output, const Settings& settings,
    const PowerService& power, const TimeService* const time, const BootStatus* const boot) {
    if (!output.write("{")) {
        return false;
    }
    bool first = true;
    for (const auto section : kAllSections) {
        if (!section_available(settings, section))
            continue;
        if ((!first && !output.write(",")) ||
            !write_section(output, settings, power, time, boot, section)) {
            return false;
        }
        first = false;
    }
    return output.write("}");
}

} // namespace

void set_state_writer(const StateWriter writer) {
#if USE_TASMOTA_STATE
    state_writer = writer;
#else
    (void)writer;
#endif
}

void set_mqtt_status_reader(const MqttStatusReader reader) {
#if USE_TASMOTA_STATE && USE_MQTT
    mqtt_status_reader = reader;
#else
    (void)reader;
#endif
}

void set_status_request_fallback([[maybe_unused]] const bool fallback) {
#if USE_TASMOTA_STATE && USE_MQTT
    request_fallback = fallback;
#endif
}

bool finish_status_split_request([[maybe_unused]] const bool deliver) {
#if USE_TASMOTA_STATE && USE_MQTT
    if (requested_split.sections == 0U)
        return false;
    if (deliver)
        (active_split.sections == 0U ? active_split : repeat_split) = requested_split;
    requested_split = {};
    return true;
#else
    return false;
#endif
}

bool next_status_split_section(
    [[maybe_unused]] std::uint8_t& section, [[maybe_unused]] bool& fallback) {
#if USE_TASMOTA_STATE && USE_MQTT
    if (active_split.sections == 0U) {
        active_split = repeat_split;
        repeat_split = {};
    }
    if (active_split.sections == 0U)
        return false;
    section = 0U;
    while ((active_split.sections & (1U << section)) == 0U)
        ++section;
    active_split.sections = static_cast<std::uint16_t>(active_split.sections & ~(1U << section));
    fallback = active_split.fallback;
    return true;
#else
    return false;
#endif
}

std::string_view status_response_suffix(
    [[maybe_unused]] const std::string_view payload, const std::string_view suffix) {
#if USE_TASMOTA_STATE
    constexpr std::string_view kSensorKey = "{\"StatusSNS\":";
    if (payload.substr(0U, kSensorKey.size()) == kSensorKey) {
        return sensor_section == 8U ? "STATUS8" : "STATUS10";
    }
#endif
    return suffix;
}

/**
 * @brief Dispatches indexed, numeric-payload, and aggregate Status queries.
 */
CommandStatus dispatch_status(const CommandRequest& request, const Settings& settings,
    const PowerService& power, const TimeService* const time, const BootStatus* const boot,
    CommandOutput& output) {
    if (!support::ascii_iequals(request.name, "status")) {
        return CommandStatus::unknown;
    }

    std::uint8_t section = 0;
    bool all = false;
    if (request.has_index) {
        if (!request.is_query() || request.index > kMaximumStatus) {
            return CommandStatus::invalid;
        }
        section = static_cast<std::uint8_t>(request.index);
        all = section == 0;
    } else if (!request.is_query()) {
        std::uint32_t parsed = 0;
        if (!support::parse_unsigned_decimal(request.payload, kMaximumStatus, parsed)) {
            return CommandStatus::invalid;
        }
        section = static_cast<std::uint8_t>(parsed);
        all = section == 0;
    }
    if (!all && !section_available(settings, section)) {
        return CommandStatus::invalid;
    }
#if USE_TASMOTA_STATE
    if (section == 8U || section == 10U) {
        sensor_section = section;
    }
#endif

#if USE_TASMOTA_STATE && USE_MQTT
    if (all && settings.mqtt_settings().enabled) {
        requested_split = {};
        requested_split.fallback = request.source == CommandSource::mqtt && request_fallback;
        for (const auto available : kAllSections) {
            if (section_available(settings, available))
                requested_split.sections =
                    static_cast<std::uint16_t>(requested_split.sections | (1U << available));
        }
        if (request.source == CommandSource::mqtt)
            return CommandStatus::handled;
    }
#endif

    const bool okay = all ? write_all(output, settings, power, time, boot)
                          : write_one(output, settings, power, time, boot, section);
    return okay ? CommandStatus::handled : CommandStatus::failed;
}

} // namespace newmota::commands
