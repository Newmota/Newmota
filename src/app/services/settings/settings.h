/*
  settings.h - persistent settings interface for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  StateText presentation reference: Tasmota support_command.ino/support_tasmota.ino,
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
  MaxPower/SO33 reference: Tasmota xdrv_03_esp32_energy.ino, settings.ino and
  support_command.ino, Copyright (C) Theo Arends, GPL version 3 or later.
*/

/** @file settings.h
 * @brief Transactional tagged settings, including retained feature presentation records.
 */

#pragma once

#include "newmota_features.h"

// A build may set its own default through device_name.
#ifndef NEWMOTA_DEVICE_NAME
#define NEWMOTA_DEVICE_NAME "Newmota"
#endif

#include "services/light/light_settings.h"
#include "services/energy/energy_presentation.h"
#if USE_SENSOR_SETTINGS
#include "drivers/sensors/sensor_settings.h"
#endif

#include "core/hardware/hardware.h"
#include "core/storage/record_slots.h"
#include "services/mqtt/mqtt_settings.h"
#include "services/power/power_settings.h"
#include "services/telemetry/telemetry_settings.h"
#include "services/timers/timer_settings.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace newmota {

#if USE_SENSOR_SETTINGS
struct SensorPresentation;
#endif

enum class SettingsLoadResult : std::uint8_t {
    loaded,
    recovered,
    defaults,
    corrupt,
    unavailable,
    failed,
};

enum class SettingsUpdateResult : std::uint8_t {
    saved,
    pending,
    unchanged,
    invalid,
    failed,
};

struct TimeRule final {
    std::uint8_t hemisphere;
    std::uint8_t week;
    std::uint8_t month;
    std::uint8_t day_of_week;
    std::uint8_t hour;
    std::int16_t offset_minutes;
};

class Settings final {
  public:
    static constexpr std::uint16_t kOptionCount = 178;
    static constexpr std::uint16_t kHighestReportedOption = 165;
    static constexpr std::uint8_t kWifiSlotCount = 2;
    static constexpr std::size_t kSsidCapacity = 32;
    static constexpr std::size_t kPasswordCapacity = 64;
    static constexpr std::size_t kDeviceNameCapacity = 32;
    static constexpr std::size_t kHostnameCapacity = 32;
    static constexpr std::size_t kOtaUrlCapacity = 192;
    static constexpr std::uint8_t kNtpServerCount = 3;
    static constexpr std::size_t kNtpServerCapacity = 63;
    static constexpr std::uint8_t kTimeRuleCount = 2;
    static constexpr std::uint8_t kStandardTimeRule = 0;
    static constexpr std::uint8_t kDaylightTimeRule = 1;
    static constexpr std::uint8_t kDefaultSerialLogLevel = 2;
    static constexpr std::uint8_t kDefaultWebLogLevel = 2;
    static constexpr std::uint8_t kMaximumLogLevel = 4;
    /** @brief Tasmota ButtonDebounce defaults/range; units digit 9 requires native AC probing. */
    static constexpr std::uint16_t kDefaultButtonDebounce = 50;
    static constexpr std::uint16_t kMinimumButtonDebounce = 40;
    static constexpr std::uint16_t kMaximumButtonDebounce = 1000;
    /** @brief Tasmota MAX_SWITCHES_SET and the SwitchMode values 0..16. */
    static constexpr std::size_t kSwitchCount = 32;
    static constexpr std::uint8_t kSwitchModeCount = 17;
    /** @brief Tasmota MAX_SWITCHES_TXT; each SwitchText holds at most 15 characters, so the
     * full SwitchText listing fits one 1024-byte response.
     */
    static constexpr std::size_t kSwitchTextCount = 28;
    static constexpr std::size_t kSwitchTextCapacity = 16;
    /** @brief Tasmota MAX_TUYA_FUNCTIONS: TuyaMCU function to data point pairs. */
    static constexpr std::size_t kTuyaFunctionCount = 16;

    /** @brief One `TuyaMCU fnId,dpId` pair; function 0 marks a free entry. */
    struct TuyaFunction final {
        std::uint8_t function{};
        std::uint8_t dp{};

        [[nodiscard]] constexpr bool operator==(const TuyaFunction& other) const {
            return function == other.function && dp == other.dp;
        }
    };

    /** @brief TuyaMCU settings kept by every profile, as the switch settings are. */
    struct TuyaSettings final {
        std::array<TuyaFunction, kTuyaFunctionCount> functions{};
        bool stat_topic{}; /**< TuyaSend9: publish stat DpType<t>Id<d>, Tasmota tuyamcu_topic. */
        std::uint16_t dimmer_min{0U}; /**< DimmerRange: MCU value of 0 %, Tasmota dimmer_hw_min. */
        std::uint16_t dimmer_max{100U}; /**< DimmerRange: MCU value of 100 %, dimmer_hw_max. */
        std::uint8_t rgb_type{};        /**< TuyaRGB 0..3; Tasmota keeps it as function 230. */
        std::uint8_t temp_set_res{1U};  /**< TuyaTempSetRes 0..3, Tasmota temperature_set_res. */
        /** TuyaEnumList: highest value of Enum1..4, 0..31; Tasmota keeps them as 231..234. */
        std::array<std::uint8_t, 4> enum_max{};

        [[nodiscard]] bool operator==(const TuyaSettings& other) const {
            return functions == other.functions && stat_topic == other.stat_topic &&
                   dimmer_min == other.dimmer_min && dimmer_max == other.dimmer_max &&
                   rgb_type == other.rgb_type && temp_set_res == other.temp_set_res &&
                   enum_max == other.enum_max;
        }
    };

    /** @brief Tasmota's Sleep default and maximum, in milliseconds per loop pass. */
    static constexpr std::uint8_t kDefaultSleep = 50;
    static constexpr std::uint8_t kMaximumSleep = 250;
    /** @brief Longest main-loop wait; GPIO button probes continue inside the idle wait. */
    static constexpr std::uint8_t kMaximumActiveSleep = 50;
    /** @brief Saved WifiPower selecting the chip's default; `WifiPower 1` in Tasmota's terms. */
    static constexpr std::uint8_t kWifiPowerChipDefault = 10;
    static constexpr std::uint16_t kDefaultTelePeriodSeconds =
        TelemetrySettings::kDefaultPeriodSeconds;
    static constexpr std::string_view kDefaultDeviceName = NEWMOTA_DEVICE_NAME;
    static constexpr std::string_view kDefaultHostname = "%s-%04d";

    /** @brief Returns the compiled-in SSID default for one Wi-Fi slot. */
    [[nodiscard]] static constexpr std::string_view default_ssid(std::uint8_t) { return {}; }

    /** @brief Returns the compiled-in password default for one Wi-Fi slot. */
    [[nodiscard]] static constexpr std::string_view default_password(std::uint8_t) { return {}; }

    /** @brief Returns the compiled-in NTP address for one zero-based slot. */
    [[nodiscard]] static constexpr std::string_view default_ntp_server(const std::uint8_t slot) {
        switch (slot) {
        case 0:
            return "2.pool.ntp.org";
        case 1:
            return "2.europe.pool.ntp.org";
        case 2:
            return "2.nl.pool.ntp.org";
        default:
            return {};
        }
    }

    /** @brief Shared defaults: SO32 key hold40, SO33 retry5, SO34 hold200, SO42 Celsius90.
     * Stored zero values are preserved.
     */
    static constexpr std::array<std::uint8_t, 18> kDefaultParameters{
        40, 5, 200, 0, 0, 0, 0, 0, 0, 0, 90};

    struct Data final {
        std::array<std::uint32_t, 5> flags{};
        std::array<std::uint8_t, 18> parameters = kDefaultParameters;
        std::array<std::array<char, kSsidCapacity>, kWifiSlotCount> ssids{};
        std::array<std::uint8_t, kWifiSlotCount> ssid_lengths{};
        std::array<std::array<char, kPasswordCapacity>, kWifiSlotCount> passwords{};
        std::array<std::uint8_t, kWifiSlotCount> password_lengths{};
        std::uint8_t preferred_wifi_slot{};
        std::array<char, kPasswordCapacity> web_password{};
        std::uint8_t web_password_length{};
        std::array<TimeRule, kTimeRuleCount> time_rules{
            TimeRule{0, 0, 10, 1, 3, 60},
            TimeRule{0, 0, 3, 1, 2, 120},
        };
        std::int16_t timezone_offset_minutes{60};
        std::uint8_t time_format{};
        // Retained by every profile, including Safeboot without a Time service.
        std::array<std::array<char, kNtpServerCapacity>, kNtpServerCount> ntp_servers{};
        std::array<std::uint8_t, kNtpServerCount> ntp_server_lengths{};
        bool timezone_automatic{};
        std::uint8_t serial_log_level{kDefaultSerialLogLevel};
        std::uint8_t web_log_level{kDefaultWebLogLevel};
        std::uint16_t tele_period_seconds{kDefaultTelePeriodSeconds};
        energy::OutputSettings energy_output{};
        std::array<std::array<char, 32>, 4>
            state_text{};                         /**< Empty entries retain source defaults. */
        std::uint8_t global_temperature_source{}; /**< Retained GlobalTemp2 field ordinal0..250. */
        std::uint8_t sleep{kDefaultSleep};        /**< Sleep: loop pass target and Wi-Fi saving. */
        std::uint8_t wifi_power{kWifiPowerChipDefault};        /**< WifiPower in tenths of a dBm. */
        std::uint16_t button_debounce{kDefaultButtonDebounce}; /**< Filter flags and handler ms. */
        std::uint16_t switch_debounce{kDefaultButtonDebounce}; /**< SwitchDebounce, same rules. */
        std::array<std::uint8_t, kSwitchCount> switch_modes{}; /**< SwitchMode1..32, Tasmota 0. */
        std::array<std::array<char, kSwitchTextCapacity>, kSwitchTextCount>
            switch_texts{}; /**< SwitchText1..28; empty means Switch<x>. */
        TuyaSettings tuya{};
        std::array<char, kDeviceNameCapacity> device_name{};
        std::uint8_t device_name_length{};
        std::array<char, kHostnameCapacity> hostname{};
        std::uint8_t hostname_length{};
        std::array<char, kOtaUrlCapacity> ota_url{};
        std::uint8_t ota_url_length{};
        bool ota_update_pending{};
        MqttSettings mqtt{};
        PowerSettings power{};
        LightSettings light{};
#if USE_TIMERS
        TimerSettings timers{};
#else
        // Safeboot preserves the application's timer record without linking its policy parser.
        std::array<std::uint8_t, TimerSettings::kRecordSize> timer_record{};
        bool timer_record_present{};
#endif
#if USE_SENSOR_SETTINGS
        SensorSettings sensors{};
#endif
        HardwareConfiguration hardware{};
    };

    /** @brief Constructs settings with target-specific hardware defaults. */
    Settings();

    /**
     * @brief Loads and validates the active and recovery settings files.
     * @return The selected settings source or failure state.
     */
    [[nodiscard]] SettingsLoadResult load();
    /**
     * @brief Reads a boolean or byte-valued SetOption.
     * @return true when @p index is supported and @p value was set.
     */
    [[nodiscard]] bool option(std::uint16_t index, std::uint8_t& value) const;
    /**
     * @brief Persists one SetOption value transactionally.
     * @return The update outcome.
     */
    [[nodiscard]] SettingsUpdateResult set_option(std::uint16_t index, std::uint8_t value);
    /**
     * @brief Returns the SSID stored in zero-based @p slot.
     */
    [[nodiscard]] std::string_view ssid(std::uint8_t slot) const;
    /**
     * @brief Returns the Wi-Fi password stored in zero-based @p slot.
     */
    [[nodiscard]] std::string_view password(std::uint8_t slot) const;

    /** @brief Returns the persisted Wi-Fi slot preferred at startup. */
    [[nodiscard]] std::uint8_t preferred_wifi_slot() const { return data_.preferred_wifi_slot; }

    /**
     * @brief Persists the SSID for zero-based @p slot.
     * @return The update outcome.
     */
    [[nodiscard]] SettingsUpdateResult set_ssid(
        std::uint8_t slot, std::string_view value, bool select = false);
    /**
     * @brief Persists the Wi-Fi password for zero-based @p slot.
     * @return The update outcome.
     */
    [[nodiscard]] SettingsUpdateResult set_password(
        std::uint8_t slot, std::string_view value, bool select = false);
    /**
     * @brief Persists station credentials and the web credential as one update.
     *
     * The selected slot is made preferred by default.  An empty web
     * credential is valid and means that the web service has no login gate.
     */
    [[nodiscard]] SettingsUpdateResult set_wifi_configuration(std::uint8_t slot,
        std::string_view ssid, std::string_view password, std::string_view web_password,
        bool select = true);
    /**
     * @brief Replaces station credentials and web password in one transaction.
     * @return The update outcome without partially persisting either slot.
     */
    [[nodiscard]] SettingsUpdateResult set_wifi_credentials(
        const std::array<std::string_view, kWifiSlotCount>& ssids,
        const std::array<std::string_view, kWifiSlotCount>& passwords,
        std::string_view web_password, std::uint8_t preferred_slot);
    /** @brief Persists the preferred startup Wi-Fi slot. */
    [[nodiscard]] SettingsUpdateResult set_preferred_wifi_slot(std::uint8_t slot);
    /** @brief Returns the web credential; an empty value disables authentication. */
    [[nodiscard]] std::string_view web_password() const;
    /** @brief Persists a web credential of 8..64 bytes, or clears authentication. */
    [[nodiscard]] SettingsUpdateResult set_web_password(std::string_view value);
    /** @brief Returns the persisted web/UI device label. */
    [[nodiscard]] std::string_view device_name() const;
    /** @brief Persists the bounded web/UI device label. */
    [[nodiscard]] SettingsUpdateResult set_device_name(std::string_view value);
    /** @brief Returns the persisted hostname template or literal. */
    [[nodiscard]] std::string_view hostname() const;
    /** @brief Persists the bounded hostname template or literal. */
    [[nodiscard]] SettingsUpdateResult set_hostname(std::string_view value);
    /** @brief Returns the operator-selected native update image URL. */
    [[nodiscard]] std::string_view ota_url() const;
    /** @brief Persists an HTTP update URL or an empty value to clear it. */
    [[nodiscard]] SettingsUpdateResult set_ota_url(std::string_view value);

    /** @brief Reports a URL update queued for the Safeboot handoff. */
    [[nodiscard]] bool ota_update_pending() const { return data_.ota_update_pending; }

    /** @brief Persists or clears the pending URL update handoff. */
    [[nodiscard]] SettingsUpdateResult set_ota_update_pending(bool pending);

    /** @brief Returns the selected Time command response format. */
    [[nodiscard]] std::uint8_t time_format() const { return data_.time_format; }

    /** @brief Reports whether Timezone 99 automatic DST rules are selected. */
    [[nodiscard]] bool timezone_automatic() const { return data_.timezone_automatic; }

    /** @brief Returns the configured fixed UTC offset in minutes. */
    [[nodiscard]] std::int16_t timezone_offset_minutes() const {
        return data_.timezone_offset_minutes;
    }

    /** @brief Returns one standard or daylight-saving transition rule. */
    [[nodiscard]] const TimeRule& time_rule(std::uint8_t index) const {
        return data_.time_rules[index];
    }

    /** @brief Persists the selected Time response format. */
    [[nodiscard]] SettingsUpdateResult set_time_format(std::uint8_t format);
    /** @brief Returns one zero-based NTP address; an empty address disables that slot. */
    [[nodiscard]] std::string_view ntp_server(std::uint8_t slot) const;
    /** @brief Commits a DNS hostname, IPv4 address, or empty disabled NTP slot. */
    [[nodiscard]] SettingsUpdateResult set_ntp_server(std::uint8_t slot, std::string_view value);
    /** @brief Persists a fixed UTC offset or automatic timezone selection. */
    [[nodiscard]] SettingsUpdateResult set_timezone(std::int16_t offset_minutes, bool automatic);

    /** @brief Returns the configured serial log threshold. */
    [[nodiscard]] std::uint8_t serial_log_level() const { return data_.serial_log_level; }

    /** @brief Returns the configured future web log threshold. */
    [[nodiscard]] std::uint8_t web_log_level() const { return data_.web_log_level; }

    /** @brief Persists the serial log threshold. */
    [[nodiscard]] SettingsUpdateResult set_serial_log_level(std::uint8_t level);
    /** @brief Persists the future web log threshold. */
    [[nodiscard]] SettingsUpdateResult set_web_log_level(std::uint8_t level);

#if USE_TIMERS
    /** @brief Returns persisted civil timer policy. */
    [[nodiscard]] const TimerSettings& timer_settings() const { return data_.timers; }

    /** @brief Persists a complete validated timer candidate transactionally. */
    [[nodiscard]] SettingsUpdateResult set_timer_settings(const TimerSettings& value);
#endif

    /** @brief Returns the persistent MQTT broker and publication settings. */
    [[nodiscard]] const MqttSettings& mqtt_settings() const { return data_.mqtt; }
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    /** @brief Requests another bounded best-effort retained ButtonTopic clear, even if OFF. */
    void request_button_retain_clear() { ++button_retain_clear_generation_; }

    /** @brief Requests a bounded best-effort retained SwitchTopic clear, as SwitchRetain OFF. */
    void request_switch_retain_clear() { ++switch_retain_clear_generation_; }

    [[nodiscard]] std::uint32_t switch_retain_clear_generation() const {
        return switch_retain_clear_generation_;
    }

    [[nodiscard]] std::uint32_t button_retain_clear_generation() const {
        return button_retain_clear_generation_;
    }
#endif
#if USE_MQTT
    /**
     * @brief Persists one validated MQTT settings record transactionally.
     * @return The persistent update outcome.
     */
    [[nodiscard]] SettingsUpdateResult set_mqtt_settings(const MqttSettings& settings);
    [[nodiscard]] SettingsUpdateResult set_mqtt_configuration(
        const MqttSettings& settings, std::uint16_t tele_period);
#endif
    /** @brief Returns the saved Sleep value in milliseconds, 0..kMaximumSleep. */
    [[nodiscard]] std::uint8_t sleep() const { return data_.sleep; }

    /** @brief Returns the applied loop sleep, capped at kMaximumActiveSleep. */
    [[nodiscard]] std::uint8_t active_sleep() const {
        return data_.sleep < kMaximumActiveSleep ? data_.sleep : kMaximumActiveSleep;
    }

    /** @brief Persists the Sleep value; 0 disables loop sleep and Wi-Fi power saving. */
    [[nodiscard]] SettingsUpdateResult set_sleep(std::uint8_t milliseconds);

    /** @brief Returns whether a ButtonDebounce value is in range and uses a supported mode. */
    [[nodiscard]] static constexpr bool valid_button_debounce(const std::uint16_t value) {
        return value >= kMinimumButtonDebounce && value <= kMaximumButtonDebounce &&
               value % 10U != 9U;
    }

    /** @brief Returns ButtonDebounce, retained across every application/Safeboot profile. */
    [[nodiscard]] std::uint16_t button_debounce() const { return data_.button_debounce; }

    /** @brief Transactionally saves filter timing/flags; unsupported AC mode is rejected. */
    [[nodiscard]] SettingsUpdateResult set_button_debounce(std::uint16_t milliseconds);

    /** @brief Returns SwitchDebounce, retained by every profile like ButtonDebounce. */
    [[nodiscard]] std::uint16_t switch_debounce() const { return data_.switch_debounce; }

    /** @brief Saves SwitchDebounce under the ButtonDebounce rules; AC mode is rejected. */
    [[nodiscard]] SettingsUpdateResult set_switch_debounce(std::uint16_t milliseconds);

    /** @brief Returns SwitchMode1..32 by zero-based index. */
    [[nodiscard]] const std::array<std::uint8_t, kSwitchCount>& switch_modes() const {
        return data_.switch_modes;
    }

    /** @brief Saves one SwitchMode, or all of them for index 0, as Tasmota's SwitchMode0.
     * @param index One-based switch, or 0 for every switch.
     * @param mode SwitchMode value below kSwitchModeCount.
     */
    [[nodiscard]] SettingsUpdateResult set_switch_mode(std::size_t index, std::uint8_t mode);

    /** @brief Returns SwitchText<x>, empty when unset, or for an index above kSwitchTextCount.
     * @param index One-based switch.
     */
    [[nodiscard]] std::string_view switch_text(std::size_t index) const;

    /** @brief Saves SwitchText<x>; an empty value restores the Switch<x> default.
     * @param value Printable text without spaces, quotes or backslashes, at most
     *        kSwitchTextCapacity - 1 bytes, so it never needs JSON escaping.
     */
    [[nodiscard]] SettingsUpdateResult set_switch_text(std::size_t index, std::string_view value);

    /** @brief Returns the TuyaMCU function map and stat topic flag. */
    [[nodiscard]] const TuyaSettings& tuya_settings() const { return data_.tuya; }

    /** @brief Saves the TuyaMCU settings in one transaction. */
    [[nodiscard]] SettingsUpdateResult set_tuya_settings(const TuyaSettings& value);

    /** @brief Returns the saved WifiPower: 0 dynamic, kWifiPowerChipDefault, or tenths of a dBm. */
    [[nodiscard]] std::uint8_t wifi_power() const { return data_.wifi_power; }

    /** @brief Persists WifiPower; the caller applies the chip's limit. */
    [[nodiscard]] SettingsUpdateResult set_wifi_power(std::uint8_t tenths_dbm);

    /** @brief Returns the saved global temperature field ordinal, including excluded features. */
    [[nodiscard]] std::uint8_t global_temperature_source() const {
        return data_.global_temperature_source;
    }

    /** @brief Durably selects an ordinal without changing runtime state on save failure.
     * @param source Zero for automatic observations;1..250 for configured field selection.
     * @return saved/unchanged, invalid input, or failed durable write.
     */
    [[nodiscard]] SettingsUpdateResult set_global_temperature_source(std::uint8_t source);

    /** @brief Returns a retained state alias; out-of-range indexes use ON as pinned.
     * @param index Zero-based OFF/ON/TOGGLE/HOLD index.
     * @return Read-only alias valid until the next Settings mutation.
     */
    [[nodiscard]] std::string_view state_text(unsigned index) const;
    /** @brief Atomically stores one nonempty printable alias, replacing spaces with underscores.
     * @param index Zero-based state index0..3.
     * @param value At most31 bytes; control characters/oversize are rejected, never truncated.
     * @return Saved/unchanged, invalid shape, or persistence failure.
     */
    [[nodiscard]] SettingsUpdateResult set_state_text(unsigned index, std::string_view value);

    /** @brief Returns source energy formatting settings independently of feature selection. */
    [[nodiscard]] const energy::OutputSettings& energy_output_settings() const {
        return data_.energy_output;
    }

    /** @brief Persists validated presentation without changing runtime on save failure.
     * @param settings Source decimal-place settings, independent of measurement/accounting units.
     * @return saved/unchanged, invalid input, or failed durable write.
     */
    [[nodiscard]] SettingsUpdateResult set_energy_output_settings(
        const energy::OutputSettings& settings);

#if USE_SENSOR_SETTINGS
    /** @brief Returns the persisted per-driver probe policy, including uncompiled IDs. */
    [[nodiscard]] const SensorSettings& sensor_settings() const { return data_.sensors; }

    /** @brief Returns persisted precision and compensation choices for sensor output. */
    [[nodiscard]] const SensorOutputSettings& sensor_output_settings() const {
        return data_.sensors.output;
    }

    /** @brief Projects persisted output choices with the active SetOption unit selections. */
    [[nodiscard]] SensorPresentation sensor_presentation() const;

    /** @brief Persists shared precision and compensation choices in app-owned storage. */
    [[nodiscard]] SettingsUpdateResult set_sensor_output_settings(
        const SensorOutputSettings& settings);

    /** @brief Persists one stable sensor driver ID without changing runtime on failure. */
    [[nodiscard]] SettingsUpdateResult set_sensor_driver_enabled(std::uint8_t id, bool enabled);
#endif

    /** @brief Returns the shared periodic telemetry interval in seconds. */
    [[nodiscard]] std::uint16_t tele_period_seconds() const { return data_.tele_period_seconds; }

    /**
     * @brief Persists the shared periodic telemetry interval.
     * @return The persistent update outcome.
     */
    [[nodiscard]] SettingsUpdateResult set_tele_period_seconds(std::uint16_t seconds);

    /** @brief Requests one immediate telemetry publication without persistence. */
    void request_telemetry() {
        ++telemetry_request_generation_;
        if (telemetry_request_generation_ == 0U) {
            telemetry_request_generation_ = 1U;
        }
    }

    /** @brief Returns the non-persistent telemetry request generation. */
    [[nodiscard]] std::uint32_t telemetry_request() const { return telemetry_request_generation_; }

    /** @brief Persists one validated standard or daylight transition rule. */
    [[nodiscard]] SettingsUpdateResult set_time_rule(std::uint8_t index, const TimeRule& rule);

    /** @brief Returns the periodic relay-state save interval in seconds. */
    [[nodiscard]] std::uint16_t save_data_seconds() const { return data_.power.save_data_seconds; }

    /** @brief Returns the selected Tasmota PowerOnState value. */
    [[nodiscard]] std::uint8_t power_on_state() const { return data_.power.power_on_state; }

    /** @brief Returns the last successfully persisted logical relay mask. */
    [[nodiscard]] std::uint32_t saved_power_state() const { return data_.power.saved_state; }

    [[nodiscard]] const LightSettings& light_settings() const { return data_.light; }

    [[nodiscard]] SettingsUpdateResult set_light_settings(
        const LightSettings& value, bool immediate = false);

    /** @brief Returns all persistent power-service settings. */
    [[nodiscard]] const PowerSettings& power_settings() const { return data_.power; }

    /**
     * @brief Persists a SaveData interval together with current relay state.
     * @return The persistent update outcome.
     */
    [[nodiscard]] SettingsUpdateResult set_save_data(
        std::uint16_t seconds, std::uint32_t current_power);
    /**
     * @brief Persists one validated Tasmota PowerOnState value.
     * @return The persistent update outcome.
     */
    [[nodiscard]] SettingsUpdateResult set_power_on_state(std::uint8_t value);
    /** @brief Persists one Tasmota-encoded relay pulse duration. */
    [[nodiscard]] SettingsUpdateResult set_pulse_time(std::uint8_t index, std::uint16_t value);
    /** @brief Persists the relay blink half-period in tenths of a second. */
    [[nodiscard]] SettingsUpdateResult set_blink_time(std::uint16_t value);
    /** @brief Persists the finite relay blink count, or zero for continuous. */
    [[nodiscard]] SettingsUpdateResult set_blink_count(std::uint16_t value);
    /** @brief Persists Tasmota LedState (0–8). */
    [[nodiscard]] SettingsUpdateResult set_led_state(std::uint8_t value);
    /** @brief Persists Tasmota LedMask. */
    [[nodiscard]] SettingsUpdateResult set_led_mask(std::uint16_t value);
    /** @brief Persists LedPwmMode (bits 0–3), LedPwmOn, and LedPwmOff together. */
    [[nodiscard]] SettingsUpdateResult set_led_pwm(
        std::uint8_t mask, std::uint8_t on_level, std::uint8_t off_level);
    /** @brief Persists validated interlock enablement and group masks. */
    [[nodiscard]] SettingsUpdateResult set_interlock(
        bool enabled, const std::array<std::uint32_t, kMaximumInterlockGroups>& groups);
    /**
     * @brief Advances periodic relay-state persistence by one second.
     * @return The persistent update outcome when due, otherwise unchanged.
     */
    [[nodiscard]] SettingsUpdateResult poll_power_state(std::uint32_t current_power);
    /**
     * @brief Persists current relay state before an orderly restart.
     * @return The persistent update outcome.
     */
    [[nodiscard]] SettingsUpdateResult flush_power_state(std::uint32_t current_power);

    /** @brief Returns the persistent module, Template, and GPIO candidate. */
    [[nodiscard]] const HardwareConfiguration& hardware_configuration() const {
        return data_.hardware;
    }

    /**
     * @brief Strictly validates and atomically persists a hardware candidate.
     * @return The update outcome without changing live state on failure.
     */
    [[nodiscard]] SettingsUpdateResult set_hardware_configuration(
        const HardwareConfiguration& configuration);

    /** @brief Returns Tasmota's default standard or daylight transition rule. */
    [[nodiscard]] static constexpr TimeRule default_time_rule(const std::uint8_t index) {
        return index == kDaylightTimeRule ? TimeRule{0, 0, 3, 1, 2, 120}
                                          : TimeRule{0, 0, 10, 1, 3, 60};
    }

    /**
     * @brief Replaces settings with defaults and optionally preserves Wi-Fi.
     *
     * When @p erase_first is true, all Newmota settings records are removed
     * before the new default record is written.
     *
     * @return The persistent update outcome.
     */
    [[nodiscard]] SettingsUpdateResult reset_to_defaults(bool preserve_wifi, bool erase_first);
    /**
     * @brief Identifies byte-valued SetOption indices.
     * @return true for the SetOption32 through SetOption49 range.
     */
    [[nodiscard]] static bool is_parameter_option(std::uint16_t index);

    /**
     * @brief Returns the generation of the active settings record.
     */
    [[nodiscard]] std::uint32_t generation() const { return generation_; }

    /**
     * @brief Records the settings that the current runtime has applied.
     *
     * Later persistent edits can then be distinguished from the active
     * hardware, Wi-Fi, and startup-policy configuration.
     */
    void mark_runtime_applied();

    void require_pwm_restart() { pwm_restart_required_ = true; }

    /** @brief Reports whether an explicit Restart 1 is needed to apply edits. */
    [[nodiscard]] bool restart_required() const;

    /** @brief Reports that ordinary writes would discard unreadable settings. */
    [[nodiscard]] bool read_only() const { return read_only_ || critical_read_only_; }

  private:
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    std::uint32_t button_retain_clear_generation_{};
    std::uint32_t switch_retain_clear_generation_{};
#endif
    /**
     * @brief Writes a candidate settings record into the slot not holding generation_.
     * @return true when the candidate became the active record.
     */
    [[nodiscard]] bool save(const Data& candidate, bool explicit_reset = false);
    /** @brief Writes and verifies the authoritative OTA-critical Wi-Fi record. */
    [[nodiscard]] bool save_critical_wifi(const Data& candidate, bool explicit_reset = false);
    /**
     * @brief Writes the selected logical power mask when it has changed.
     * @return The persistent update outcome.
     */
    [[nodiscard]] SettingsUpdateResult persist_power_state(std::uint32_t current_power);

    Data data_{};
    std::uint32_t generation_{0};
    std::uint32_t critical_generation_{0};
    std::uint16_t save_data_counter_{1};
    /** @brief Index of the settings slot holding generation_, or kNoRecordSlot. */
    std::uint8_t active_slot_{kNoRecordSlot};
    bool read_only_{false};
    bool critical_read_only_{false};
    bool suppress_power_save_until_restart_{false};
    std::uint32_t telemetry_request_generation_{};
    HardwareConfiguration applied_hardware_{};
    std::array<std::array<char, kSsidCapacity>, kWifiSlotCount> applied_ssids_{};
    std::array<std::uint8_t, kWifiSlotCount> applied_ssid_lengths_{};
    std::array<std::array<char, kPasswordCapacity>, kWifiSlotCount> applied_passwords_{};
    std::array<std::uint8_t, kWifiSlotCount> applied_password_lengths_{};
    std::uint8_t applied_preferred_wifi_slot_{};
    std::uint8_t applied_set_option_153_{};
    std::uint8_t applied_set_option_15_{};
    std::uint8_t applied_set_option_68_{};
    std::uint8_t applied_set_option_37_{};
    bool light_dirty_{};
    bool pwm_restart_required_{};
    bool runtime_applied_{false};
};

} // namespace newmota
