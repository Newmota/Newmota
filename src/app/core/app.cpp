/*
  app.cpp - application runtime for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Primary Power/SetOption21 reference: Tasmota xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later.
  Modbus user-field precision reference: Tasmota xnrg_29_modbus.ino,
  Copyright (C) 2022 Theo Arends, GPL version 3 or later.
*/

/** @file app.cpp
 * @brief Initializes shared application services and runs the cooperative firmware loop.
 */

#include "newmota_features.h"
#include "newmota/app.h"
#include "services/settings/state_text.h"
#if USE_GLOBAL_TEMPERATURE
#include "services/temperature/global_temperature.h"
#endif
#if USE_GLOBAL_TEMPERATURE_COMMANDS
#include "services/temperature/global_temperature_selection.h"
#endif
#include "core/command/command.h"
#if USE_DEFER
#include "core/command/defer.h"
#endif
#if USE_HA_DISCOVERY
#include "services/mqtt/ha_discovery.h"
#endif
#include "core/command/serial_console.h"
#include "core/hardware/hardware.h"
#if USE_BERRY || USE_SERIAL || USE_WS2812 || USE_DISPLAY || USE_I2S || USE_EDGE_CAPTURE ||         \
    USE_ADC || USE_DEVICE_SPI || USE_ADE7953_EM || USE_ADE7953_PLUS2PM
#include "core/hardware/runtime_pin_claims.h"
#endif
#if USE_I2S
#include "drivers/audio/audio_codecs.h"
#if USE_WIFI && (USE_AUDIO_MP3 || USE_AUDIO_AAC || USE_AUDIO_OPUS)
#include "drivers/audio/audio_network.h"
#endif
#include "drivers/audio/audio_commands.h"
#include "drivers/audio/audio_session.h"
#include "drivers/audio/audio_store.h"
#include "drivers/audio/i2s_service.h"
#if USE_BERRY_AUDIO
#include "drivers/audio/audio_level.h"
#include "drivers/audio/audio_spectrum.h"
#endif
#endif
#if USE_DISPLAY
#include "drivers/display/display_service.h"
#endif
#if USE_WS2812
#include "drivers/led_strip/led_strip.h"
#endif
#if USE_ADC
#include "drivers/adc/adc_service.h"
#endif
#if USE_SERIAL
#include "drivers/serial/serial_service.h"
#endif
#if USE_MODBUS_RTU
#include "drivers/modbus/rtu_service.h"
#endif
#include "core/message/message.h"
#include "core/scheduler.h"
#include "core/storage/filesystem.h"
#if USE_SDCARD
#include "core/storage/sd_card_mount.h"
#endif
#include "core/system/boot_counter.h"
#include "core/system/boot_guard.h"
#include "core/system/loop_sleep.h"
#include "core/command/result_timestamp.h"
#include "core/system/restart.h"
#if USE_DEEPSLEEP
#include "core/system/deep_sleep_service.h"
#endif
#if USE_BERRY_ULP
#include "services/berry/modules/ulp/ulp_module.h"
#endif
#include "core/system/uptime.h"
#if USE_WEB_CONSOLE
#include "newmota/console_history.h"
#endif
#include "newmota/log.h"
#include "newmota/platform/platform.h"
#if USE_DISPLAY
#include "newmota/platform/display.h"
#endif
#if USE_OTA_PULL
#include "newmota/platform/tcp_stream.h"
#include "services/update/url_update_service.h"
#endif
#if USE_WGET
#include "services/network/wget_service.h"
#endif
#if USE_ESP_HOSTED
#include "services/network/hosted_service.h"
#endif
#if USE_ETHERNET
#include "services/network/ethernet_service.h"
#endif
#if USE_PING
#include "services/network/ping_service.h"
#endif
#if USE_WIFI_POWER
#include "services/network/wifi_tx_power.h"
#endif
#include "services/settings/settings.h"
#include "services/power/power_service.h"
#if USE_PWM || USE_WS2812
#include "services/light/light_service.h"
#endif
#if USE_ESP_TASMOTA_SETTINGS_IMPORT
#include "services/migration/tasmota_settings_import.h"
#endif
#if USE_WEB
#include "services/web/web_service.h"
#endif
#if USE_RECOVERY_WEB
#include "services/web/recovery_web_service.h"
#endif
#if USE_RECOVERY_WEB || USE_BEKEN_UPDATE_WEB
#include "newmota/platform/update.h"
#include "core/update/update_session.h"
#include "services/web/update_web_service.h"
#endif
#if USE_HARDWARE_COMMANDS
#include "gpio_ids.h"
#include "hardware_names.h"
#endif
#if USE_WIFI
#include "services/network/network_service.h"
#include "services/network/network_commands.h"
#endif
#if USE_MQTT
#include "newmota/platform/tcp_stream.h"
#include "services/mqtt/mqtt_client.h"
#if USE_BERRY
#include "services/mqtt/berry/mqtt_module.h"
#endif
#include "services/mqtt/mqtt_commands.h"
#include "services/mqtt/mqtt_service.h"
#endif
#if USE_TIMERS
#include "services/timers/timer_service.h"
#endif
#if USE_BLE_SENSORS
#include "drivers/sensors/ble_sensors.h"
#endif
#if USE_TIME
#include "services/time/time_commands.h"
#include "services/time/time_service.h"
#endif
#if USE_TASMOTA_STATE
#include "services/power/power_telemetry.h"
#include "services/status/status_commands.h"
#endif
#if USE_NTP_SERVER
#include "newmota/platform/ntp_server.h"
#endif
#if USE_BERRY
#include "services/berry/autoexec.h"
#if USE_MATTER
#include "services/matter/berry/matter_module.h"
#endif
#include "services/berry/berry_service.h"
#if USE_BERRY_DRIVERS
#include "services/berry/modules/newmota/drivers.h"
#endif
#endif

#if USE_I2C
#include "drivers/i2c/i2c_service.h"
#endif
#if USE_CAMERA
#include "drivers/camera/camera_service.h"
#endif
#if USE_GPS
#include "drivers/gps/gps_service.h"
#endif
#if USE_BLE
#include "newmota/platform/ble.h"
#include "services/ble/ble_advertisements.h"
#endif
#if USE_RTC_CHIPS
#include "drivers/rtc/rtc_service.h"
#endif
#if USE_SENSORS
#include "drivers/sensors/sensor_history.h"
#include "drivers/sensors/sensor_service.h"
#endif

#if USE_EDGE_CAPTURE
#include "drivers/gpio/edge_capture_service.h"
#endif
#if USE_ENERGY
#include "services/energy/energy_service.h"
#endif
#if USE_SENSORS || USE_ENERGY || USE_SWITCHES
#include "services/telemetry/measurement_document.h"
#endif
#if USE_TUYA_MCU
#include "drivers/tuya/tuya_mcu.h"
#endif

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdint>
#if USE_MQTT && USE_APP_MESSAGE_ROUTING
#include <cstdlib>
#include <new>
#endif
#include <string_view>

namespace newmota {
namespace {

constexpr std::uint32_t kIdleSleepMilliseconds = 1;

#if USE_WIFI && USE_SLEEP
/**
 * @brief Applies Tasmota's Wi-Fi sleep rule once Wi-Fi runs and whenever it changes: Sleep 0
 * or SetOption127 1 turn power saving off, any other Sleep selects the platform's modem sleep.
 */
void ApplyWifiPowerSave(const Settings& settings) {
    static int applied = -1;
    std::uint8_t no_sleep = 0U;
    (void)settings.option(127U, no_sleep);
    const int wanted = settings.sleep() != 0U && no_sleep == 0U ? 1 : 0;
    const auto state = platform::wifi_state();
    if (wanted == applied || state == platform::WifiState::disabled ||
        state == platform::WifiState::initializing)
        return;
    (void)platform::set_wifi_power_save(wanted != 0);
    applied = wanted;
}
#endif

#if USE_BERRY || USE_ENERGY || USE_SDCARD
/** @brief Flushes application-owned persisted values before a controlled restart. */
void save_application_state_before_restart(void* const context) {
#if USE_BERRY
    auto* const service = static_cast<BerryService*>(context);
    if (service != nullptr) {
        (void)service->save_persist();
    }
#else
    (void)context;
#endif
#if USE_ENERGY
    if (!energy::service().prepare_restart())
        AddLog(LogLevel::error, "NRG: restart checkpoint failed");
#endif
#if USE_SDCARD
    filesystem::unmount_sd(); // Last: Berry may still have written to the card above.
#endif
}
#endif

// The firmware runtime is a singleton. Keep its fixed-capacity state in BSS
// instead of consuming the ESP-IDF main task stack for the lifetime of run().
Settings settings;
HardwareRuntime hardware_runtime;
RestartController restart;
#if USE_WIFI
NetworkService network{settings, &restart};
#endif
#if USE_TIME
TimeService time_service{settings};
#endif
BootCounter boot_counter;
PowerService power;

#ifndef NEWMOTA_IMAGE_SAFEBOOT
#if USE_SWITCHES
/** @brief Returns the policy the switch handler and switch reports read from settings. */
SwitchPolicy CurrentSwitchPolicy() {
    std::uint8_t detached = 0U;
    std::uint8_t hold_tenths = 40U;
    (void)settings.option(114U, detached);
    (void)settings.option(32U, hold_tenths);
    return {settings.switch_modes().data(), detached != 0U, hold_tenths};
}
#endif

/** @brief Keeps button and switch probes running during main passes and cooperative idle waits.
 */
void PollInputs(const std::uint32_t now) {
    if (power.button_count() != 0U) {
        std::uint8_t hold_tenths = 40U;
        (void)settings.option(32U, hold_tenths);
        std::uint8_t immediate = 0U;
        std::uint8_t swap_single_double = 0U;
        std::uint8_t decoupled = 0U;
        (void)settings.option(13U, immediate);
        (void)settings.option(11U, swap_single_double);
        (void)settings.option(73U, decoupled);
        power.poll_buttons(now,
            {immediate != 0U, swap_single_double != 0U, decoupled != 0U, hold_tenths},
            settings.button_debounce());
    }
#if USE_SWITCHES
    if (power.switch_count() != 0U)
        power.poll_switches(now, CurrentSwitchPolicy(), settings.switch_debounce());
#endif
}
#endif
#if USE_DISPLAY
bool display_poll_fault_logged{};
#endif
#if USE_TIMERS
TimerService timers{settings, time_service, power};
#endif
#if USE_PWM || USE_WS2812
LightService light;
#endif
#if USE_APP_MESSAGE_ROUTING
MessageRouter messages;
#endif
#if USE_RECOVERY_WEB || USE_BEKEN_UPDATE_WEB
UpdateWebService update_web{platform::browser_update_writer(), restart};
#endif
#if USE_RECOVERY_WEB
RecoveryWebService recovery_web{settings, update_web};
#endif
#if USE_OTA_PULL
#if USE_RECOVERY_WEB || USE_BEKEN_UPDATE_WEB
UpdateSession url_update_session{platform::browser_update_writer()};
UpdateSession* const url_update_session_ptr = &url_update_session;
#else
UpdateSession* const url_update_session_ptr = nullptr;
#endif
UrlUpdateService url_update{
    settings, restart, platform::tcp_stream(), url_update_session_ptr, USE_BEKEN_UPDATE_WEB != 0};
#endif
#if USE_BERRY
#if USE_MQTT
BerryMqttBridge berry_mqtt;
#endif
BerryService berry;
AutoexecService autoexec{berry};
#endif
#if USE_WGET
WgetService wget{platform::tcp_stream()};
#endif
#if USE_PING
PingService ping;
#endif
CommandProcessor command_processor{settings, restart, power
#if USE_TIME
    ,
    time_service
#endif
#if USE_BERRY
    ,
    berry
#endif
    ,
    &boot_counter
#if USE_WIFI
    ,
    &network
#endif
#if USE_OTA_PULL
    ,
    &url_update
#endif
#if USE_WGET
    ,
    &wget
#endif
#if USE_PING
    ,
    &ping
#endif
};
#if USE_MQTT
/** @brief Owns MQTT working memory only while persisted settings enable it. */
struct MqttRuntime final {
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    std::uint32_t button_clear_generation{};
#if USE_SWITCHES
    std::uint32_t switch_clear_generation{};
#endif
#endif
#if !USE_APP_MESSAGE_ROUTING
    MessageRouter owned_messages{};
#endif
    MessageRouter* messages{};
    MqttClient client;
    MqttService service;
    MqttCommandService commands;
    MqttSettings applied_settings{};
    std::uint16_t applied_tele_period_seconds{};
    std::uint32_t applied_telemetry_request{};
    bool configuration_known{};
    bool telemetry_configuration_known{};

    explicit MqttRuntime(CommandProcessor& command_processor
#if USE_APP_MESSAGE_ROUTING
        ,
        MessageRouter& shared_messages
#endif
        )
#if USE_APP_MESSAGE_ROUTING
        : messages(&shared_messages),
#else
        : messages(&owned_messages),
#endif
          client(platform::tcp_stream()), service(*messages, client.transport()),
          commands(*messages, command_processor, service) {
    }

    ~MqttRuntime() {
        service.set_enabled(false);
        client.disconnect();
    }

    [[nodiscard]] MessageRouter& message_router() { return *messages; }
};

#if USE_APP_MESSAGE_ROUTING
MqttRuntime* mqtt_runtime{};
#else
// Safeboot prioritizes image size over idle RAM, so keep its recovery runtime
// in static storage and avoid heap-allocation machinery in the recovery image.
MqttRuntime mqtt_runtime_storage{command_processor};
MqttRuntime* mqtt_runtime{&mqtt_runtime_storage};
#endif
bool mqtt_runtime_enabled{};
#endif
SerialConsole console;

#if NEWMOTA_DEEP_SLEEP
/** @brief Tasmota's WifiShutdown before deep sleep: send what MQTT holds, then close it. */
void ShutdownNetworkForDeepSleep(void*) {
#if USE_MQTT
    if (mqtt_runtime_enabled && mqtt_runtime != nullptr) {
        auto& runtime = *mqtt_runtime;
        const auto started = platform::monotonic_milliseconds();
        while ((runtime.service.publications_pending() || runtime.client.publication_pending()) &&
               platform::monotonic_milliseconds() - started < 2000U) {
            const auto now = platform::monotonic_milliseconds();
            runtime.client.poll(now);
            runtime.service.poll(now);
            platform::sleep_for_milliseconds(10U);
        }
        runtime.client.disconnect();
        platform::sleep_for_milliseconds(100U); // Flush anything in the network buffers
    }
#endif
}
#endif
#if USE_WEB
WebService web{settings, command_processor, messages, &network
#if USE_BEKEN_UPDATE_WEB
    ,
    &update_web
#else
    ,
    nullptr
#endif
#if USE_BERRY
    ,
    &berry
#else
    ,
    nullptr
#endif
    ,
    &boot_counter};
#endif

#ifndef NEWMOTA_IMAGE_SAFEBOOT
/** @brief Observes each gesture locally and routes ButtonTopic keys. */
bool RouteButtonEvent(void*, const ButtonEvent& event) {
#if USE_BERRY_DRIVERS
    // Tasmota packs any_key as device_save<<24 | key<<16 | state<<8 | device (key 0 = button)
    // and button_multi_pressed as count<<8 | zero-based index.
    if (event.send_key) {
        const std::uint32_t device_save = event.key_instance;
        auto device = device_save;
#if USE_MQTT
        // SendKey limits a button routed to its ButtonTopic to the Power endpoints.
        const auto& mqtt_settings = settings.mqtt_settings();
        const auto topic = mqtt_settings.button_topic();
        if (mqtt_runtime != nullptr && mqtt_settings.enabled &&
            mqtt_runtime->client.state() == MqttTransportState::connected && !topic.empty() &&
            topic != "0" && device > power.endpoint_count())
            device = 1U;
#endif
        queue_berry_driver_event(
            BerryDriverHook::any_key, device_save << 24 | std::uint32_t(event.state) << 8 | device);
    }
    if (event.observe && !event.immediate && event.action >= ButtonAction::single &&
        event.action <= ButtonAction::penta)
        queue_berry_driver_event(BerryDriverHook::button_multi_pressed,
            std::uint32_t(event.action) << 8 | std::uint32_t(event.instance - 1U));
#endif
    const auto* action = button_action_name(event.action);
    if (event.observe)
        AddLog(LogLevel::info, "BTN: Button%u %s", unsigned(event.instance), action);
#if USE_MQTT
    if (mqtt_runtime != nullptr) {
        std::uint8_t discovery{}, force_local{}, no_hold_retain{};
        (void)settings.option(19U, discovery);
        (void)settings.option(61U, force_local);
        (void)settings.option(62U, no_hold_retain);
        bool on{};
        const auto key = event.key_instance > power.endpoint_count() ? 1U : event.key_instance;
        (void)power.state(key, on);
        const bool consumed = mqtt_runtime->service.route_button_key(
            settings.mqtt_settings(), event, on, force_local != 0U, no_hold_retain != 0U);
        if (event.decoupled && discovery == 0U) {
            // Source SO73 status publications carry Action only; HOLD uses StateText4.
            std::array<char, 128> payload{};

            struct Buffer {
                char* data;
                std::size_t size{};
            } buffer{payload.data()};

            CommandOutput output{[](void* context, std::string_view text) {
                                     auto& b = *static_cast<Buffer*>(context);
                                     if (text.size() > 128U - b.size)
                                         return false;
                                     std::memcpy(b.data + b.size, text.data(), text.size());
                                     b.size += text.size();
                                     return true;
                                 },
                &buffer};
            char suffix[16]{};
            std::snprintf(suffix, sizeof suffix, "BUTTON%u", unsigned(event.instance));
            if (!output.format("{\"Button%u\":{\"Action\":", unsigned(event.instance)) ||
                !output.json_string(event.action == ButtonAction::hold
                                        ? state_text(3U)
                                        : std::string_view{action}) ||
                !output.write("}}") || settings.mqtt_settings() != mqtt_runtime->applied_settings ||
                !mqtt_runtime->service.queue_button_action(suffix, {payload.data(), buffer.size}))
                AddLog(LogLevel::warning, "BTN: Button%u MQTT Action dropped",
                    unsigned(event.instance));
        }
        return consumed;
    }
#endif
    return false;
}
#endif

#if USE_SWITCHES
#if USE_MESSAGE_ROUTING
void PublishSensorSnapshot(MessageRouter& message_router, std::uint32_t now);
#endif

/** @brief Writes SwitchText<x>, or Switch<x> when unset, as Tasmota's GetSwitchText. */
bool WriteSwitchName(CommandOutput& output, const std::uint8_t instance) {
    const auto text = settings.switch_text(instance);
    return text.empty() ? output.format("\"Switch%u\"", unsigned(instance))
                        : output.json_string(text);
}

/** @brief Writes `"Switch1":"ON"` for every configured switch, as Tasmota's MqttShowSensor.
 * Switches past half the snapshot are left out, as remote sensors are, so SENSOR still fits.
 */
bool WriteSwitchFields(CommandOutput& output) {
    constexpr std::size_t kBudget = CanonicalMessage::kPayloadCapacity / 2U;
    static bool omitted_logged = false;
    const auto policy = CurrentSwitchPolicy();
    const auto start = output.size();
    bool first = true;
    for (std::uint8_t instance = 1U; instance <= BasicIoDriver::kMaximumSwitches; ++instance) {
        bool on = false;
        if (!power.switch_state(instance, policy, on))
            continue;
        const auto write_field = [&](CommandOutput& target) {
            return (first || target.write(",")) && WriteSwitchName(target, instance) &&
                   target.write(":") && target.json_string(state_text(on ? 1U : 0U));
        };
        CommandOutput counter{[](void*, std::string_view) { return true; }, nullptr};
        (void)write_field(counter);
        if (output.size() - start + counter.size() > kBudget) {
            if (!omitted_logged)
                AddLog(LogLevel::warning, "SWT: Switch%u and later left out of SENSOR",
                    unsigned(instance));
            omitted_logged = true;
            return true;
        }
        if (!write_field(output))
            return false;
        first = false;
    }
    omitted_logged = false;
    return true;
}

/** @brief Returns the highest configured switch, the range of retained SwitchTopic keys. */
std::uint8_t HighestSwitch() {
    // Configured switches, so an MCU switch that has not reported yet is still cleared.
    const auto configured = power.configured_switches();
    std::uint8_t highest = 0U;
    for (std::uint8_t instance = 1U; instance <= BasicIoDriver::kMaximumSwitches; ++instance)
        if ((configured & (1U << (instance - 1U))) != 0U)
            highest = instance;
    return highest;
}

/** @brief Routes switch SendKey events, SetOption114 Actions and SwitchMode 15/16 reports, as
 * Tasmota's SendKey and SwitchHandler do. A connected SwitchTopic consumes the key.
 */
bool RouteSwitchEvent(void*, const SwitchEvent& event) {
    if (event.kind == SwitchEventKind::report) {
        // Tasmota publishes SENSOR at once; refresh the snapshot so it carries the new state.
#if USE_APP_MESSAGE_ROUTING
        PublishSensorSnapshot(messages, platform::monotonic_milliseconds());
#elif USE_MESSAGE_ROUTING && USE_MQTT
        if (mqtt_runtime)
            PublishSensorSnapshot(
                mqtt_runtime->message_router(), platform::monotonic_milliseconds());
#endif
#if USE_MQTT
        if (mqtt_runtime != nullptr)
            mqtt_runtime->service.request_sensor_telemetry();
#endif
        return false;
    }
#if USE_BERRY_DRIVERS
    // Key 1 = switch. Tasmota shows detached SetOption114 Actions only to Rules; Newmota gives
    // them their own switch_action hook with the same packing, leaving any_key unchanged.
    queue_berry_driver_event(event.kind == SwitchEventKind::action ? BerryDriverHook::switch_action
                                                                   : BerryDriverHook::any_key,
        std::uint32_t(event.instance) << 24 | 1U << 16 | std::uint32_t(event.state) << 8 |
            event.instance);
#endif
    const bool action = event.kind == SwitchEventKind::action;
    std::uint8_t discovery{};
    (void)settings.option(19U, discovery);
    // SetOption19 suppresses the whole SetOption114 Action, including its key.
    if (action && discovery != 0U)
        return false;
    bool consumed = false;
    if (!action || event.state <= 2U) {
        AddLog(LogLevel::info, "SWT: Switch%u State %u", unsigned(event.instance),
            unsigned(event.state));
#if USE_MQTT
        if (mqtt_runtime != nullptr) {
            std::uint8_t force_local{}, no_hold_retain{};
            (void)settings.option(61U, force_local);
            (void)settings.option(62U, no_hold_retain);
            bool on{};
            (void)power.state(event.instance, on);
            consumed = mqtt_runtime->service.route_switch_key(settings.mqtt_settings(),
                {event.instance, event.state, SwitchEventKind::key}, on, force_local != 0U,
                no_hold_retain != 0U);
        }
#endif
    }
    if (!action)
        return consumed;
    static constexpr const char* kPressStates[] = {"POWER_INCREMENT", "POWER_INV", "POWER_CLEAR",
        "POWER_RELEASE", "POWER_100", "", "POWER_DELAYED"};
    const std::string_view word = event.state <= 3U ? state_text(event.state)
                                  : event.state <= 10U
                                      ? std::string_view{kPressStates[event.state - 4U]}
                                      : std::string_view{};
    AddLog(LogLevel::info, "SWT: Switch%u Action %.*s", unsigned(event.instance),
        static_cast<int>(word.size()), word.data());

#if USE_MQTT
    std::array<char, 96> payload{};

    struct Buffer {
        char* data;
        std::size_t size{};
    } buffer{payload.data()};

    CommandOutput output{[](void* context, std::string_view text) {
                             auto& b = *static_cast<Buffer*>(context);
                             if (text.size() > 96U - b.size)
                                 return false;
                             std::memcpy(b.data + b.size, text.data(), text.size());
                             b.size += text.size();
                             return true;
                         },
        &buffer};
    if (!output.write("{") || !WriteSwitchName(output, event.instance) ||
        !output.write(":{\"Action\":") || !output.json_string(word) || !output.write("}}"))
        return false;
    char suffix[16]{};
    std::snprintf(suffix, sizeof suffix, "SWITCH%u", unsigned(event.instance));
    if (mqtt_runtime != nullptr &&
        (settings.mqtt_settings() != mqtt_runtime->applied_settings ||
            !mqtt_runtime->service.queue_button_action(suffix, {payload.data(), buffer.size})))
        AddLog(LogLevel::warning, "SWT: Switch%u MQTT Action dropped", unsigned(event.instance));
#endif
    return false;
}
#endif

#if USE_OTA_PULL
/** @brief Excludes OTA from concurrent filesystem downloads and browser uploads. */
bool filesystem_transfer_active(void*) {
    return false
#if USE_WEB
           || web.filesystem_upload_active()
#endif
#if USE_WGET
           || wget.active()
#endif
        ;
}
#endif
#if USE_WGET
/** @brief Checks leases before reserving storage and the shared stream. */
bool wget_busy(void*) {
    return restart.pending()
#if USE_WEB
           || web.filesystem_upload_active()
#endif
#if USE_RECOVERY_WEB || USE_BEKEN_UPDATE_WEB
           || update_web.active()
#endif
#if USE_OTA_PULL
           || url_update.active() || url_update.state() == UrlUpdateState::waiting_network ||
           url_update.state() == UrlUpdateState::handoff || settings.ota_update_pending()
#endif
        ;
}
#endif

#if USE_STATUS_LEDS
/** @brief Reads LedState, LedMask, and SetOption31 for the LED driver. */
LedPolicy CurrentLedPolicy() {
    LedPolicy policy{
        settings.power_settings().led_state, settings.power_settings().led_mask, false};
    std::uint8_t no_problem_blink = 0U;
    (void)settings.option(31U, no_problem_blink);
    policy.no_problem_blink = no_problem_blink != 0U;
    policy.pwm_mask = settings.power_settings().led_pwm_mask;
    policy.pwm_on = settings.power_settings().led_pwm_on;
    policy.pwm_off = settings.power_settings().led_pwm_off;
#if USE_PWM
    policy.carrier_hz = settings.light_settings().frequency;
#endif
    return policy;
}

/** @brief Samples power, connectivity, and pending restart/update state for the LEDs. */
LedInputs CurrentLedInputs() {
    LedInputs inputs{};
    inputs.power = power.state_mask();
#if USE_WIFI
    inputs.network_down = !network.online();
#endif
#if USE_MQTT
    // Like Tasmota, MQTT counts as down only while it is enabled and not connected.
    if (mqtt_runtime_enabled && mqtt_runtime != nullptr) {
        inputs.mqtt_down = mqtt_runtime->client.state() != MqttTransportState::connected;
        const auto stats = mqtt_runtime->client.stats();
        inputs.mqtt_published = stats.publish_queued;
        inputs.mqtt_received = stats.received_publishes;
    }
#endif
    inputs.hold_lit = restart.pending()
#if USE_RECOVERY_WEB || USE_BEKEN_UPDATE_WEB
                      || update_web.active()
#endif
#if USE_OTA_PULL
                      || url_update.active()
#endif
        ;
    return inputs;
}
#endif

#if USE_TUYA_MCU
/** @brief Returns what the MCU's Wi-Fi LED shows, as Tasmota's TuyaGetTuyaWifiState. */
tuya::LinkState TuyaLinkState() {
    auto link = tuya::LinkState::offline;
#if USE_WIFI
    if (network.access_point())
        link = tuya::LinkState::access_point;
    else
#endif
        if (restart.pending())
        link = tuya::LinkState::restarting;
#if USE_MQTT
    if (mqtt_runtime != nullptr && mqtt_runtime->client.state() == MqttTransportState::connected)
        link = tuya::LinkState::cloud;
#endif
    return link;
}

/** @brief Publishes TuyaReceived on tele RESULT (TUYARECEIVED with SetOption4), the raw data
 * point values on stat and reported enums on stat RESULT, as Tasmota; without MQTT the text is
 * logged.
 */
void PublishTuya(void*, const tuya::Publication kind, const std::string_view leaf,
    const std::string_view payload) {
#if USE_MQTT
    if (mqtt_runtime != nullptr) {
        const bool queued =
            kind == tuya::Publication::received
                ? mqtt_runtime->service.queue_telemetry_event(
                      settings.mqtt_settings().response_topic ? "TUYARECEIVED" : "RESULT", payload)
                : mqtt_runtime->service.queue_status_event(
                      leaf, payload, kind == tuya::Publication::result);
        if (queued)
            return;
    }
#else
    (void)kind;
    (void)leaf;
#endif
    AddLog(LogLevel::debug, "TYA: %.*s", static_cast<int>(payload.size()), payload.data());
}
#endif

#if USE_HARDWARE_COMMANDS
/** @brief Finds the selected application module's generated display name. */
std::string_view SelectedModuleName(const HardwareConfiguration& configuration) {
    if (configuration.module == 0U) {
        return configuration.user_template_name();
    }
    for (std::size_t index = 0; index < generated::gpio::kModules.size(); ++index) {
        if (generated::gpio::kModules[index].number == configuration.module) {
            const auto offset = generated::gpio::kModuleNameOffsets[index];
            return &generated::gpio::kModuleNames[offset];
        }
    }
    return "Unknown";
}
#endif

#if USE_PING
/**
 * @brief Adapts CommandOutput writes to the active platform console.
 * @return true when the complete fragment was written.
 */
bool WriteConsole(void*, const std::string_view text) {
    return WriteConsoleResult(text);
}

/** @brief Admits an asynchronous Ping result into the shared ordered response queue. */
bool PublishPingResult(void* context, std::string_view payload) {
    auto& router = *static_cast<MessageRouter*>(context);
    MessageMetadata metadata{};
    metadata.kind = MessageKind::response;
    if (!metadata.set_source("ping") || !metadata.set_response_topic_suffix("PING"))
        return false;
    return router.publish(metadata, payload) == MessagePublishResult::published;
}
#endif

#if USE_WIFI
/** @brief Delivers one retained asynchronous Wi-Fi scan result per surface. */
void PublishWifiScanAnnouncements(MessageRouter* message_router) {
    std::array<char, 384> payload{};
    std::size_t payload_size = 0U;
    std::size_t network_number = 0U;
    const platform::WifiScanRecord* result = nullptr;

    if (network.next_wifi_scan_announcement(WifiScanDelivery::console, network_number, result)) {
        if (commands::serialize_wifi_scan_announcement(
                network_number, result, payload.data(), payload.size(), payload_size)) {
            (void)WriteConsoleResult({payload.data(), payload_size});
            (void)platform::write_console("\r\n");
            network.acknowledge_wifi_scan_announcement(WifiScanDelivery::console);
        }
    }

#if USE_MESSAGE_ROUTING
    if (message_router == nullptr) {
        network.acknowledge_wifi_scan_announcement(WifiScanDelivery::message);
        return;
    }
    result = nullptr;
    if (!network.next_wifi_scan_announcement(WifiScanDelivery::message, network_number, result)) {
        return;
    }
    payload_size = 0U;
    if (!commands::serialize_wifi_scan_announcement(
            network_number, result, payload.data(), payload.size(), payload_size)) {
        AddLog(LogLevel::error, "WIF: scan-result serialization failed");
        return;
    }
    MessageMetadata metadata{};
    metadata.kind = MessageKind::response;
    if (!metadata.set_source("wifi")) {
        AddLog(LogLevel::error, "WIF: scan-result source metadata failed");
        return;
    }
    const auto published = message_router->publish(metadata, {payload.data(), payload_size});
    if (published == MessagePublishResult::published ||
        published == MessagePublishResult::replaced_snapshot) {
        network.acknowledge_wifi_scan_announcement(WifiScanDelivery::message);
    }
#else
    network.acknowledge_wifi_scan_announcement(WifiScanDelivery::message);
#endif
}
#endif

#if USE_TASMOTA_STATE
/** @brief Writes a Tasmota duration such as "Uptime":"1T02:03:04". */
bool WriteDuration(CommandOutput& output, const char* const name, const std::uint32_t seconds) {
    return output.format("\"%s\":\"%luT%02lu:%02lu:%02lu\"", name,
        static_cast<unsigned long>(seconds / 86400U),
        static_cast<unsigned long>(seconds / 3600U % 24U),
        static_cast<unsigned long>(seconds / 60U % 60U), static_cast<unsigned long>(seconds % 60U));
}

/**
 * @brief Writes Tasmota's STATE fields in MqttShowState order, without braces. Fields whose
 * value the platform cannot report (a Wi-Fi Mode, BSSId before the first link refresh) are
 * left out rather than guessed.
 */
bool WriteStateFields(CommandOutput& output) {
    const auto now = platform::monotonic_milliseconds();
    const auto uptime = uptime_seconds();
#if USE_TIME
    if (!commands::write_state_time(output, settings, time_service, now) || !output.write(","))
        return false;
#endif
    std::uint32_t mqtt_count = 0U;
#if USE_MQTT
    if (mqtt_runtime != nullptr)
        mqtt_count = mqtt_runtime->client.stats().connections;
#endif
    if (!WriteDuration(output, "Uptime", uptime) ||
        !output.format(",\"UptimeSec\":%lu,\"Heap\":%lu", static_cast<unsigned long>(uptime),
            static_cast<unsigned long>(platform::memory_info().heap_free_bytes / 1024U)) ||
#if USE_SLEEP
        !output.write(",") || !write_sleep_state_fields(output) ||
#endif
        !output.format(",\"MqttCount\":%lu", static_cast<unsigned long>(mqtt_count)))
        return false;
#if USE_BERRY
    std::uint32_t heap_used = 0U, objects = 0U;
    if (berry.memory(heap_used, objects) &&
        !output.format(",\"Berry\":{\"HeapUsed\":%lu,\"Objects\":%lu}",
            static_cast<unsigned long>(heap_used), static_cast<unsigned long>(objects)))
        return false;
#endif
    if (!write_power_state_fields(output, power, false))
        return false;
    const char* address = "0.0.0.0";
#if USE_WIFI
    const auto info = platform::network_info();
    if (info.address[0] != '\0')
        address = info.address;
    if (network.connected()) {
        const auto slot = network.station_slot();
        if (!output.format(",\"Wifi\":{\"AP\":%u,\"SSId\":", static_cast<unsigned>(slot + 1U)) ||
            !output.json_string(network.station_ssid()))
            return false;
        if (const auto* link = network.link()) {
            const auto* b = link->bssid;
            if (!output.format(",\"BSSId\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"Channel\":%u", b[0],
                    b[1], b[2], b[3], b[4], b[5], static_cast<unsigned>(link->channel)) ||
                (platform::wifi_band(link->channel) != nullptr &&
                    !output.format(",\"Band\":\"%s\"", platform::wifi_band(link->channel))) ||
                (link->mode != nullptr && !output.format(",\"Mode\":\"%s\"", link->mode)))
                return false;
        }
        // Tasmota reports the 0..100 quality as RSSI and the dBm value as Signal.
        if (const auto rssi = network.link_rssi())
            if (!output.format(",\"RSSI\":%d,\"Signal\":%d", wifi_rssi_quality(*rssi),
                    static_cast<int>(*rssi)))
                return false;
        if (!output.format(
                ",\"LinkCount\":%lu,", static_cast<unsigned long>(network.link_count())) ||
            !WriteDuration(output, "Downtime", network.downtime_seconds()) || !output.write("}"))
            return false;
    }
#endif
    return output.write(",\"Hostname\":") &&
           output.json_string(
#if USE_WIFI
               network.hostname()
#else
               ""
#endif
                   ) &&
           output.format(",\"IPAddress\":\"%s\"", address);
}
#endif

#if USE_MESSAGE_ROUTING
/**
 * @brief Publishes the complete relay STATE as one canonical snapshot.
 *
 * The application owns the typed power state and serializes it once.  WebUI
 * and future transports consume the resulting immutable payload rather than
 * rebuilding another presentation-specific state document.
 */
void PublishStateSnapshot(MessageRouter& message_router, const std::uint32_t now) {
    std::array<char, CanonicalMessage::kPayloadCapacity> payload{};
    std::size_t length = 0U;
#if USE_TASMOTA_STATE
    struct Buffer {
        char* data;
        std::size_t capacity, size;
    } buffer{payload.data(), payload.size(), 0U};

    CommandOutput output{[](void* context, const std::string_view text) {
                             auto& target = *static_cast<Buffer*>(context);
                             if (text.size() > target.capacity - target.size)
                                 return false;
                             std::memcpy(target.data + target.size, text.data(), text.size());
                             target.size += text.size();
                             return true;
                         },
        &buffer};
    if (!output.write("{") || !WriteStateFields(output) || !output.write("}")) {
        AddLog(LogLevel::error, "MSG: state snapshot serialization failed");
        return;
    }
    length = buffer.size;
#else
    if (!power.serialize_state(uptime_seconds(), payload.data(), payload.size(), length)) {
        AddLog(LogLevel::error, "MSG: state snapshot serialization failed");
        return;
    }
#endif

    MessageMetadata metadata{};
    metadata.kind = MessageKind::snapshot;
    metadata.revision = power.state_revision();
    metadata.measured_at_ms = now;
    if (!metadata.set_source("power")) {
        AddLog(LogLevel::error, "MSG: state source metadata failed");
        return;
    }
    const auto result = message_router.publish(metadata, {payload.data(), length});
    if (result == MessagePublishResult::invalid) {
        AddLog(LogLevel::error, "MSG: state snapshot rejected");
    }
}
#endif

#if USE_MESSAGE_ROUTING && (USE_SENSORS || USE_ENERGY || USE_SWITCHES)
/** @brief Publishes the complete sensor document as one canonical snapshot. */
void PublishSensorSnapshot(MessageRouter& message_router, const std::uint32_t now) {
    static bool capacity_warning_active = false;
    // Leave room for the serializer's terminating NUL beyond the routed payload.
    std::array<char, CanonicalMessage::kPayloadCapacity + 1U> payload{};
    std::size_t length = 0U;
    bool capacity_exceeded = false;
    if (!telemetry::serialize_measurements(
            payload.data(), payload.size(), length, &capacity_exceeded)) {
        if (capacity_exceeded) {
            if (!capacity_warning_active) {
                AddLog(LogLevel::warning,
                    "MSG: warning: sensor snapshot exceeds %u-byte payload capacity",
                    static_cast<unsigned>(CanonicalMessage::kPayloadCapacity));
            }
            capacity_warning_active = true;
        } else {
            AddLog(LogLevel::error, "MSG: sensor snapshot serialization failed");
        }
        return;
    }
    capacity_warning_active = false;

    std::uint32_t revision = 2166136261U;
    for (std::size_t index = 0U; index < length; ++index) {
        revision = (revision ^ static_cast<std::uint8_t>(payload[index])) * 16777619U;
    }
    if (revision == 0U)
        revision = 1U;

    MessageMetadata metadata{};
    metadata.kind = MessageKind::snapshot;
    metadata.revision = revision;
    metadata.measured_at_ms = now;
    if (!metadata.set_source("sensors")) {
        AddLog(LogLevel::error, "MSG: sensor source metadata failed");
        return;
    }
    const auto result = message_router.publish(metadata, {payload.data(), length});
    if (result == MessagePublishResult::invalid ||
        result == MessagePublishResult::snapshot_slots_full) {
        AddLog(LogLevel::error, "MSG: sensor snapshot rejected");
    }
}
#endif

/**
 * @brief Mounts LittleFS and logs its availability and capacity.
 *
 * Mount failures are reported but do not prevent the firmware from starting.
 */
void MountFilesystem() {
    const auto result = filesystem::mount();
    if (result == filesystem::MountResult::mounted ||
        result == filesystem::MountResult::already_mounted) {
        std::uint32_t total_capacity = 0;
        if (filesystem::total_capacity(total_capacity)) {
            AddLog(LogLevel::info, "UFS: LittleFS total %lu KiB",
                static_cast<unsigned long>(total_capacity / 1024));
        } else {
            AddLog(LogLevel::error, "UFS: LittleFS capacity unavailable");
        }
        return;
    }

    switch (result) {
    case filesystem::MountResult::unavailable:
        AddLog(LogLevel::info, "UFS: LittleFS storage not configured");
        break;
    case filesystem::MountResult::invalid_device:
        AddLog(LogLevel::error, "UFS: invalid LittleFS storage geometry");
        break;
    case filesystem::MountResult::mount_failed:
        AddLog(LogLevel::error, "UFS: LittleFS mount failed (%d); not formatting",
            filesystem::last_error());
        break;
    case filesystem::MountResult::grow_failed:
        AddLog(LogLevel::error, "UFS: LittleFS grow failed (%d)", filesystem::last_error());
        break;
    case filesystem::MountResult::mounted:
    case filesystem::MountResult::already_mounted:
    case filesystem::MountResult::invalid_pins: // SD card results; flash never reports them.
    case filesystem::MountResult::no_card:
    case filesystem::MountResult::no_filesystem:
        break;
    }
}

/** @brief Resolves persistent hardware and initializes core-owned GPIO roles. */
void InitializeHardware() {
#if USE_RTC_CHIPS
    rtc::service().reset();
#endif
#if defined(NEWMOTA_IMAGE_APPLICATION)
    auto configuration = settings.hardware_configuration();
    if (boot_guard().reached(kBootLoopNoGpioCount) && configuration.override_count != 0U) {
        // Tasmota clears the user GPIOs; this boot only ignores them and keeps the settings.
        AddLog(LogLevel::error, "GPIO: boot loop; %u user GPIO overrides ignored this boot",
            static_cast<unsigned int>(configuration.override_count));
        configuration.overrides = {};
        configuration.override_count = 0U;
    }
    const auto result = hardware_runtime.initialize(configuration);
#else
    const auto result = hardware_runtime.initialize(settings.hardware_configuration());
#endif
#if USE_BERRY || USE_SERIAL || USE_WS2812 || USE_DISPLAY || USE_I2S || USE_EDGE_CAPTURE ||         \
    USE_ADC || USE_DEVICE_SPI || USE_ADE7953_EM || USE_ADE7953_PLUS2PM
    runtime_pins::initialize(&hardware_runtime.hardware());
#endif
    if (!result.resolved()) {
        AddLog(LogLevel::error, "GPIO: module %u resolution failed (%u)",
            static_cast<unsigned int>(settings.hardware_configuration().module),
            static_cast<unsigned int>(result.resolution.error));
        return;
    }
    if (result.console_pin.valid()) {
        AddLog(LogLevel::info, "CMD: serial console off, template uses GPIO%u",
            static_cast<unsigned int>(result.console_pin.value()));
    }

#if USE_HARDWARE_COMMANDS
    const auto name = SelectedModuleName(settings.hardware_configuration());
    AddLog(LogLevel::info, "GPIO: module=%u name=%.*s active=%u initialized=%u",
        static_cast<unsigned int>(hardware_runtime.hardware().module()),
        static_cast<int>(name.size()), name.data(),
        static_cast<unsigned int>(hardware_runtime.hardware().active_count()),
        static_cast<unsigned int>(result.initialized_count));
#else
    AddLog(LogLevel::info, "GPIO: module=%u active=%u initialized=%u",
        static_cast<unsigned int>(hardware_runtime.hardware().module()),
        static_cast<unsigned int>(hardware_runtime.hardware().active_count()),
        static_cast<unsigned int>(result.initialized_count));
#endif

    for (std::size_t index = 0; index < hardware_runtime.hardware().issue_count(); ++index) {
        const auto* const issue = hardware_runtime.hardware().issue(index);
        if (issue != nullptr) {
            AddLog(LogLevel::error, "GPIO: pin=%u function=%u inactive reason=%u",
                static_cast<unsigned int>(issue->pin.value()),
                static_cast<unsigned int>(issue->function), static_cast<unsigned int>(issue->code));
        }
    }
    for (std::size_t index = 0; index < result.failure_count; ++index) {
        PinInitializationFailure failure{};
        if (hardware_runtime.failure(index, failure)) {
            AddLog(LogLevel::error, "GPIO: pin=%u function=%u initialization failed reason=%u",
                static_cast<unsigned int>(failure.pin.value()),
                static_cast<unsigned int>(failure.function),
                static_cast<unsigned int>(failure.error));
        }
    }

#if USE_WEB && USE_TIME
    web.set_time(&time_service);
#endif
#if USE_BLE_SENSORS && USE_TIME
    ble_sensors_attach_time(&time_service);
#endif
#if USE_TUYA_MCU
    // Before Power starts, so the boot states of the MCU relays reach the MCU.
    if (tuya::initialize(hardware_runtime.hardware(), settings, power)) {
        tuya::set_publisher(PublishTuya, nullptr);
#if USE_TIME
        tuya::set_time_source(&time_service);
#endif
    }
#endif
    std::uint8_t save_state = 0U;
    (void)settings.option(0U, save_state);
    const auto io =
        power.initialize(hardware_runtime.hardware(), platform::monotonic_milliseconds(),
            settings.power_settings(), save_state != 0U, platform::reset_was_power_on());
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    power.set_button_event_handler(RouteButtonEvent, nullptr);
#endif
#if USE_BERRY_DRIVERS
    // Tasmota packs button_pressed as count<<24 | level<<16 | last level<<8 | zero-based index,
    // with level 0 pressed and 1 released.
    power.set_button_level_handler(
        [](void*, const std::uint8_t instance, const bool pressed, const std::uint8_t count) {
            queue_berry_driver_event(BerryDriverHook::button_pressed,
                std::uint32_t(count) << 24 | (pressed ? 0U : 1U) << 16 | (pressed ? 1U : 0U) << 8 |
                    std::uint32_t(instance - 1U));
        },
        nullptr);
    power.set_operation_handler(
        [](void*) { report_berry_driver_power(power.state_mask()); }, nullptr);
#endif
#if USE_SWITCHES
    power.set_switch_event_handler(RouteSwitchEvent, nullptr);
    if (power.switch_count() != 0U)
        telemetry::bind_switch_fields(WriteSwitchFields);
#endif
#if USE_STATUS_LEDS
    power.leds().start(CurrentLedPolicy());
#endif
#if USE_PWM || USE_WS2812
    light.initialize(
        hardware_runtime.hardware(), settings, power, platform::monotonic_milliseconds());
#if USE_WEB
    web.set_light(&light);
#endif
#if USE_TIME
    light.attach_time(time_service);
#endif
    for (std::size_t i = 0U; i < 16U; ++i) {
        if (!light.outputs().configured(i))
            continue;
        auto status = light.outputs().status(i);
        if (status == platform::PwmResult::success)
            status = light.outputs().last_operation(i);
        if (status != platform::PwmResult::success)
            AddLog(LogLevel::error, "PWM: output=%u inactive reason=%u",
                static_cast<unsigned>(i + 1U), static_cast<unsigned>(status));
    }
    if (light.last_result() != platform::PwmResult::success)
        AddLog(LogLevel::error, "LGT: state restore failed reason=%u",
            static_cast<unsigned>(light.last_result()));
    if (light.channel_count() != 0U && !light.available())
        AddLog(LogLevel::error, "LGT: Power endpoint mapping overflow");
#endif
    AddLog(io.failure_count == 0U ? LogLevel::info : LogLevel::error,
        "PWR: relays=%u buttons=%u leds=%u failures=%u", static_cast<unsigned int>(io.relay_count),
        static_cast<unsigned int>(io.button_count), static_cast<unsigned int>(io.led_count),
        static_cast<unsigned int>(io.failure_count));
}

#if USE_MQTT
#if USE_APP_MESSAGE_ROUTING
std::uint32_t mqtt_allocation_retry_at{};
#endif
bool mqtt_inactive_reported{};

#if USE_BERRY
void ReceiveMqttPublish(void* context, const MqttPublishPacket& packet) {
    if (!berry.mqtt_publish(packet))
        MqttCommandService::receive_publish(context, packet);
}
#endif

/** @brief Makes the MQTT runtime available for the selected firmware role. */
bool EnsureMqttRuntime() {
#if USE_APP_MESSAGE_ROUTING
    if (mqtt_runtime)
        return true;
    const auto now = platform::monotonic_milliseconds();
    if (mqtt_allocation_retry_at != 0U &&
        static_cast<std::int32_t>(now - mqtt_allocation_retry_at) < 0) {
        return false;
    }
    mqtt_allocation_retry_at = now + 5000U;
    void* const storage = std::malloc(sizeof(MqttRuntime));
    if (storage == nullptr) {
        if (!mqtt_inactive_reported) {
            AddLog(LogLevel::error, "MQT: runtime buffers unavailable; will retry");
            mqtt_inactive_reported = true;
        }
        return false;
    }
    auto* const allocated = ::new (storage) MqttRuntime(command_processor
#if USE_APP_MESSAGE_ROUTING
        ,
        messages
#endif
    );
    mqtt_runtime = allocated;
    mqtt_allocation_retry_at = 0U;
#else
    if (mqtt_runtime == nullptr)
        return false;
#endif
    mqtt_inactive_reported = false;
#if USE_STATE_TEXT
    mqtt_runtime->service.bind_power_snapshot(
        [](void*, std::uint32_t revision, std::uint32_t& mask) {
            if (power.state_revision() != revision)
                return false;
            mask = power.state_mask();
            return true;
        },
        nullptr);
#endif
#if USE_ENERGY_PERIOD_COMPAT || (USE_GLOBAL_TEMPERATURE_COMMANDS && (USE_SENSORS || USE_ENERGY))
    (void)mqtt_runtime->service.bind_sensor_document(
        {[](void* context, char* output, std::size_t capacity, std::size_t& size,
             std::uint32_t& token) {
#if USE_GLOBAL_TEMPERATURE_COMMANDS
             refresh_global_temperature_selection(
                 *static_cast<const Settings*>(context), platform::monotonic_milliseconds(), true);
#else
             (void)context;
#endif
#if USE_ENERGY_PERIOD_COMPAT
             return energy::service().prepare_period(token) &&
                    telemetry::serialize_measurements(output, capacity, size, nullptr, true);
#else
             token = 0;
             return telemetry::serialize_measurements(output, capacity, size);
#endif
         },
            [](void*, std::uint32_t token) {
#if USE_ENERGY_PERIOD_COMPAT
                energy::service().accept_period(token);
#else
                (void)token;
#endif
            },
            &settings});
#endif
#if USE_BERRY
    mqtt_runtime->client.set_publish_handler(&ReceiveMqttPublish, &mqtt_runtime->commands);
#else
    mqtt_runtime->client.set_publish_handler(
        &MqttCommandService::receive_publish, &mqtt_runtime->commands);
#endif
#if USE_WEB
    web.set_mqtt_services(&mqtt_runtime->service, &mqtt_runtime->client);
    web.set_mqtt_test_allowed(false);
#endif
    return true;
}

/** @brief Applies persistent settings and releases all MQTT buffers when disabled. */
void ApplyMqttConfiguration() {
    const auto& stored = settings.mqtt_settings();
    mqtt_runtime_enabled = false;
#if USE_BERRY
    berry_mqtt.bind(nullptr, nullptr);
#endif

    if (!stored.valid()) {
        if (mqtt_runtime) {
            mqtt_runtime->client.disconnect();
            mqtt_runtime->service.set_enabled(false);
        }
        AddLog(LogLevel::error, "MQT: invalid persistent MQTT settings");
        return;
    }
    if (!stored.enabled) {
        if (mqtt_runtime) {
            mqtt_runtime->client.disconnect();
            mqtt_runtime->service.reset_connection();
            mqtt_runtime->service.set_enabled(false);
#if USE_WEB
            web.set_mqtt_services(nullptr, nullptr);
            web.set_mqtt_test_allowed(false);
#endif
#if USE_APP_MESSAGE_ROUTING
#if USE_BERRY
            berry_mqtt.bind(nullptr, nullptr);
#endif
            mqtt_runtime->~MqttRuntime();
            std::free(mqtt_runtime);
            mqtt_runtime = nullptr;
#endif
        }
#if USE_APP_MESSAGE_ROUTING
        mqtt_allocation_retry_at = 0U;
#endif
        mqtt_inactive_reported = true;
#if USE_APP_MESSAGE_ROUTING
        AddLog(LogLevel::info, "MQT: disabled; runtime buffers released");
#else
        AddLog(LogLevel::info, "MQT: disabled; static runtime retained");
#endif
        return;
    }
    if (!EnsureMqttRuntime())
        return;

    auto& runtime = *mqtt_runtime;
    runtime.client.disconnect();
    runtime.service.reset_connection();
    runtime.service.set_enabled(false);

    MqttClientConfig config;
    if (!config.set_host(stored.host()) || !config.set_username(stored.username()) ||
        !config.set_password(stored.password()) || !config.set_port(stored.port)) {
        AddLog(LogLevel::error, "MQT: persistent fields rejected");
        return;
    }
    config.set_keep_alive_seconds(stored.keep_alive_seconds);
    config.set_secure(stored.secure);
    config.set_retry_seconds(stored.retry_seconds);
    config.set_timeout_seconds(stored.timeout_seconds);
    std::string_view runtime_topic = stored.topic();
#if USE_WIFI
    if (!network.refresh_mqtt_topic(stored.topic())) {
        AddLog(LogLevel::error, "MQT: topic template expansion rejected");
        return;
    }
    runtime_topic = network.mqtt_topic();
#endif
    std::array<char, 13> identity{};
    const auto net_info = platform::network_info();
    std::size_t id_size = 0U;
    for (const char c : std::string_view{net_info.mac})
        if (c != ':' && id_size + 1U < identity.size())
            identity[id_size++] = (c >= 'a' && c <= 'f') ? static_cast<char>(c - 'a' + 'A') : c;
    std::array<char, MqttSettings::kClientIdCapacity> resolved_client{};
    std::size_t client_size = 0U;
    const auto client_template = stored.client_id_template();
    if (!network_identity::resolve_topic(
            client_template, platform::device_id(), resolved_client, client_size) ||
        !config.set_client_id({resolved_client.data(), client_size}) ||
        !runtime.service.configure(stored, runtime_topic, {resolved_client.data(), client_size},
            network.hostname(), {identity.data(), id_size}) ||
        !config.set_will(runtime.service.lwt_topic(), !stored.no_retain) ||
        !runtime.client.set_config(config)) {
        AddLog(LogLevel::error, "MQT: runtime configuration rejected");
        return;
    }
    std::array<std::string_view, 6> filters{};
    for (std::size_t i = 0U; i < runtime.service.command_filter_count(); ++i)
        filters[i] = runtime.service.command_filter(i);
    if (!runtime.client.set_subscriptions(filters.data(), runtime.service.command_filter_count()))
        return;
#if USE_TASMOTA_STATE
    const char* restart_reason = platform::restart_reason_name(platform::restart_reason());
    if (restart_reason == nullptr)
        restart_reason = platform::reset_was_power_on() ? "Power On" : "Restart";
#else
    const bool restart_reason = platform::reset_was_power_on();
#endif
    runtime.service.set_connection_info(platform::info().framework_version(), net_info.address,
        restart_reason, boot_counter.status().count);
    runtime.service.set_enabled(true);
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    runtime.service.set_button_endpoint_count(power.endpoint_count());
#endif
#if USE_SWITCHES
    runtime.service.set_switch_count(HighestSwitch());
#endif
#if USE_BERRY
    berry_mqtt.bind(&runtime.client, &runtime.service);
#endif
    mqtt_runtime_enabled = true;
    mqtt_inactive_reported = false;
    AddLog(LogLevel::info, "MQT: configured broker=%.*s port=%u%s",
        static_cast<int>(stored.host().size()), stored.host().data(),
        static_cast<unsigned int>(stored.port), stored.secure ? " (TLS unavailable)" : "");
}

/** @brief Reconfigures MQTT and owns its runtime only while enabled. */
void SynchronizeMqttConfiguration() {
    const auto& stored = settings.mqtt_settings();
#if USE_APP_MESSAGE_ROUTING
    if (!stored.enabled && !mqtt_runtime) {
        mqtt_runtime_enabled = false;
        if (!mqtt_inactive_reported) {
            if (!stored.valid()) {
                AddLog(LogLevel::error, "MQT: invalid persistent MQTT settings");
            } else {
                AddLog(LogLevel::info, "MQT: disabled; no runtime buffers allocated");
            }
            mqtt_inactive_reported = true;
        }
        return;
    }
#endif

    if (!stored.valid()) {
        if (!mqtt_runtime) {
            AddLog(LogLevel::error, "MQT: invalid persistent MQTT settings");
            mqtt_inactive_reported = true;
            return;
        }
    }

#ifndef NEWMOTA_IMAGE_SAFEBOOT
    if (mqtt_runtime != nullptr &&
        mqtt_runtime->button_clear_generation != settings.button_retain_clear_generation()) {
        mqtt_runtime->service.request_button_retain_clear(platform::monotonic_milliseconds());
        mqtt_runtime->button_clear_generation = settings.button_retain_clear_generation();
    }
#endif
#if USE_SWITCHES
    if (mqtt_runtime != nullptr &&
        mqtt_runtime->switch_clear_generation != settings.switch_retain_clear_generation()) {
        mqtt_runtime->service.request_switch_retain_clear(platform::monotonic_milliseconds());
        mqtt_runtime->switch_clear_generation = settings.switch_retain_clear_generation();
    }
#endif

    if (!mqtt_runtime) {
        if (!EnsureMqttRuntime())
            return;
        mqtt_runtime->applied_settings = stored;
        mqtt_runtime->configuration_known = true;
        ApplyMqttConfiguration();
    } else if (!mqtt_runtime->configuration_known || stored != mqtt_runtime->applied_settings) {
        if (mqtt_runtime->configuration_known && (!mqtt_runtime->service.prepare_reconfigure(
                                                      stored, platform::monotonic_milliseconds()) ||
                                                     mqtt_runtime->client.publication_pending())) {
            return;
        }
        mqtt_runtime->applied_settings = stored;
        mqtt_runtime->configuration_known = true;
        ApplyMqttConfiguration();
    }

    if (!mqtt_runtime)
        return;
    auto& runtime = *mqtt_runtime;
    const auto tele_period = settings.tele_period_seconds();
    if (!runtime.telemetry_configuration_known ||
        tele_period != runtime.applied_tele_period_seconds) {
        if (!runtime.service.set_tele_period_seconds(tele_period)) {
            AddLog(LogLevel::error, "MQT: invalid TelePeriod=%u",
                static_cast<unsigned int>(tele_period));
        } else {
            runtime.applied_tele_period_seconds = tele_period;
            runtime.telemetry_configuration_known = true;
        }
    }

    const auto request = settings.telemetry_request();
    if (mqtt_runtime_enabled && request != runtime.applied_telemetry_request) {
        runtime.service.request_telemetry();
        runtime.applied_telemetry_request = request;
    }
}
#endif

} // namespace

/**
 * @brief Initializes core services and runs the cooperative firmware loop.
 */
[[noreturn]] void run() {
#if defined(NEWMOTA_IMAGE_APPLICATION)
    // First, so a crash anywhere in startup counts as a quick restart.
    boot_guard().begin(platform::monotonic_milliseconds());
#endif
#if USE_TIME
    SetLogClock([](std::uint32_t& local_seconds, std::uint16_t& millisecond) {
        const auto now = platform::monotonic_milliseconds();
        const auto snapshot = time_service.snapshot(now);
        local_seconds = snapshot.local_seconds;
        millisecond = time_service.millisecond(now);
        return snapshot.valid;
    });
#endif
#if USE_OTA_PULL
    url_update.set_filesystem_busy_probe(filesystem_transfer_active, nullptr);
#endif
#if USE_WGET
    wget.set_busy_probe(wget_busy, nullptr);
#endif
#if USE_ESP_HOSTED && defined(NEWMOTA_IMAGE_APPLICATION)
#if USE_WGET
    hosted::bind(settings, &wget, restart);
#else
    hosted::bind(settings, nullptr, restart);
#endif
#endif
    platform::initialize_console();
    const auto& platform = platform::info();
    AddLog(LogLevel::info, "NMT: Newmota %s", platform.framework_version());
    AddLog(LogLevel::info, "PLT: target=%s chip=%s sdk=%s/%s build=%s", platform.target(),
        platform.chip(), platform.sdk_name(), platform.sdk_version(), platform.build_datetime());
    AddLog(LogLevel::info, "CMD: %s", platform::console_status());
#if defined(NEWMOTA_IMAGE_APPLICATION)
    if (boot_guard().reached(kBootLoopSafebootCount)) {
        // Before the filesystem and settings: Tasmota's last step. Safeboot does not count, so
        // the count is cleared first; otherwise the first application boot after it would
        // return there at once.
        const auto result =
            platform::prepare_boot_partition_toggle(platform::AlternateImage::paired);
        if (result == platform::BootPartitionSwitchResult::prepared) {
            boot_guard().clear();
            AddLog(LogLevel::error,
                "SYS: boot loop after %u quick restarts; restarting in Safeboot",
                static_cast<unsigned int>(boot_guard().restart().count));
            platform::restart();
        }
        AddLog(LogLevel::error, "SYS: boot loop; Safeboot unavailable (%u)",
            static_cast<unsigned int>(result));
    }
    if (boot_guard().reduced())
        AddLog(LogLevel::error,
            "SYS: boot loop after %u quick restarts; BLE, Matter, LVGL autostart and autoexec.be "
            "skipped this boot",
            static_cast<unsigned int>(boot_guard().restart().count));
    else if (boot_guard().restart().count > 1U)
        AddLog(LogLevel::info, "SYS: quick restart %u",
            static_cast<unsigned int>(boot_guard().restart().count));
#endif
    MountFilesystem();
    auto settings_result = settings.load();
#if USE_ESP_TASMOTA_SETTINGS_IMPORT
    if (settings_result == SettingsLoadResult::defaults) {
        const auto migration_result = migration::import_tasmota_wifi(settings);
        switch (migration_result.status) {
        case migration::TasmotaSettingsImportStatus::imported:
            settings_result = SettingsLoadResult::loaded;
            AddLog(LogLevel::info, "MIG: imported Tasmota 15.5.0.2 credentials from %s",
                migration_result.source);
            break;
        case migration::TasmotaSettingsImportStatus::not_found:
        case migration::TasmotaSettingsImportStatus::no_wifi:
            break;
        case migration::TasmotaSettingsImportStatus::unsupported:
            AddLog(LogLevel::info, "MIG: Tasmota settings layout is not supported");
            break;
        case migration::TasmotaSettingsImportStatus::invalid:
            AddLog(LogLevel::error, "MIG: Tasmota settings record failed validation");
            break;
        case migration::TasmotaSettingsImportStatus::unavailable:
            AddLog(LogLevel::error, "MIG: Tasmota settings could not be read");
            break;
        case migration::TasmotaSettingsImportStatus::write_failed:
            AddLog(LogLevel::error, "MIG: imported Wi-Fi could not be committed");
            break;
        }
    }
#endif
    ConfigureLogLevels(settings.serial_log_level(), settings.web_log_level());
    switch (settings_result) {
    case SettingsLoadResult::loaded:
        AddLog(LogLevel::info, "CFG: settings generation %lu",
            static_cast<unsigned long>(settings.generation()));
        break;
    case SettingsLoadResult::recovered:
        AddLog(LogLevel::error, "CFG: recovered settings generation %lu",
            static_cast<unsigned long>(settings.generation()));
        break;
    case SettingsLoadResult::defaults:
        AddLog(LogLevel::info, "CFG: using defaults");
        break;
    case SettingsLoadResult::corrupt:
        AddLog(LogLevel::error, "CFG: settings corrupt; using defaults");
        break;
    case SettingsLoadResult::unavailable:
        AddLog(LogLevel::info, "CFG: persistent storage unavailable");
        break;
    case SettingsLoadResult::failed:
        AddLog(LogLevel::error, "CFG: settings read failed (%d)", filesystem::last_error());
        break;
    }

#if USE_STATE_TEXT
    bind_state_text(settings);
#endif
#if USE_GLOBAL_TEMPERATURE
    global_temperature().reset();
#if USE_GLOBAL_TEMPERATURE_COMMANDS
    global_temperature().configure_source(settings.global_temperature_source());
#endif
#endif
    InitializeHardware();
#if USE_DEEPSLEEP
    // Before the network starts: a wake with sleep left over goes straight back to sleep.
    deep_sleep::initialize(hardware_runtime.hardware());
#endif
#if USE_SDCARD
#if USE_TIME
    filesystem::set_sd_clock(
        [](void* const context) -> std::uint32_t {
            const auto& clock = *static_cast<const TimeService*>(context);
            return clock.synchronized()
                       ? clock.snapshot(platform::monotonic_milliseconds()).local_seconds
                       : 0U;
        },
        &time_service);
#endif
    // As Tasmota, right after GPIO init: Berry and autoexec.be can reach /sd from the start.
    mount_sd_card(hardware_runtime.hardware());
#endif
#if defined(NEWMOTA_IMAGE_APPLICATION)
#if USE_DEEPSLEEP
    std::uint8_t deep_sleep_boot_count = 0U; // SetOption76: count boots while deep sleep is on
    (void)settings.option(76U, deep_sleep_boot_count);
    const bool count_boot = !deep_sleep::enabled() || deep_sleep_boot_count != 0U;
#else
    const bool count_boot = true;
#endif
    if (!boot_counter.initialize(count_boot)) {
        AddLog(LogLevel::error, "SYS: persistent boot counter unavailable");
    } else {
        AddLog(LogLevel::info, "SYS: boot count %lu",
            static_cast<unsigned long>(boot_counter.status().count));
    }
#endif
#if USE_MODBUS_RTU
    modbus::initialize();
#endif
#if USE_ENERGY
#if USE_ENERGY_OVERTEMP
    energy::service().bind_overtemperature(
        [](void* context, std::uint32_t now, std::int32_t& temperature, std::uint8_t& threshold) {
            return power.state_mask() != 0 &&
                   static_cast<Settings*>(context)->option(42, threshold) &&
                   global_temperature().read(now, temperature);
        },
        [](void*) {
            const auto result = power.force_off(PowerOperationSource::overtemperature);
            return result != PowerChangeResult::failed && result != PowerChangeResult::unavailable;
        },
        &settings);
#endif
#if USE_ENERGY_HARDWARE_TOTAL
    energy::service().bind_hardware_total(
        [](void* context) {
            std::uint8_t value{};
            return static_cast<Settings*>(context)->option(72, value) && value != 0;
        },
        &settings);
#endif
#if USE_ENERGY_COUNTER_FILTER
    energy::service().bind_counter_filter(
        [](void* context) {
            std::uint8_t value{};
            (void)static_cast<Settings*>(context)->option(39, value);
            return value;
        },
        &settings);
#endif
#if USE_CSE7766
    energy::service().bind_primary_invalid_power(
        [](void* context) {
            std::uint8_t value{};
            (void)static_cast<Settings*>(context)->option(39, value);
            return value;
        },
        [](void* context, std::uint8_t value) {
            const auto result = static_cast<Settings*>(context)->set_option(39, value);
            return result != SettingsUpdateResult::invalid &&
                   result != SettingsUpdateResult::failed;
        },
        &settings);
#endif
#if USE_ENERGY_POWER_POLICY || USE_ENERGY_MARGINS || USE_BERRY_ENERGY_API
    energy::service().bind_primary_power(
        [](void* context) {
            std::uint8_t value{};
            (void)static_cast<Settings*>(context)->option(21, value);
            return value != 0 || power.state_mask() != 0;
        },
        &settings);
#if USE_ENERGY_DUMMY
    energy::service().bind_primary_power_mask([](void*) { return power.state_mask(); }, nullptr);
#endif
#endif
    energy::service().bind_daily_policy(
        [](void* context) {
            std::uint8_t value{};
            (void)static_cast<Settings*>(context)->option(162, value);
            return value != 0;
        },
        &settings);
#endif
#if USE_ENERGY_CALENDAR
    energy::service().bind_calendar_clock(
        [](void* context, std::uint32_t now, std::uint32_t& local, bool& daylight) {
            const auto snapshot = static_cast<TimeService*>(context)->snapshot(now);
            local = snapshot.local_seconds;
            daylight = snapshot.daylight;
            return snapshot.valid;
        },
        &time_service);
#endif
#if USE_ENERGY_PRECISION
    energy::service().bind_output_precision(
        [](void* context) { return static_cast<Settings*>(context)->energy_output_settings(); },
        &settings);
#endif
#if USE_MODBUS_USER_PRECISION && USE_SENSOR_SETTINGS
    energy::service().bind_modbus_ambient_precision(
        [](void* context, std::uint8_t selector) {
            const auto& precision = static_cast<Settings*>(context)->sensor_output_settings();
            return selector == 26   ? precision.temperature_resolution
                   : selector == 27 ? precision.humidity_resolution
                                    : precision.pressure_resolution;
        },
        &settings);
#endif
#if USE_ENERGY
    energy::service().bind_no_common_presentation(
        [](void* context) {
            std::uint8_t value{};
            (void)static_cast<Settings*>(context)->option(150, value);
            return value != 0;
        },
        &settings);
    energy::service().bind_phase_presentation(
        [](void* context) {
            std::uint8_t value{};
            (void)static_cast<Settings*>(context)->option(129, value);
            return value != 0;
        },
        &settings);
#endif
#if USE_ENERGY
    energy::service().initialize(platform::monotonic_milliseconds());
#if USE_ENERGY_PROTECTION_BINDINGS
    energy::service().bind_protection_power(power);
#if USE_ENERGY_PROTECTION_POLICY
    energy::service().bind_protection_policy(settings, time_service);
#endif
#endif
#endif
#if USE_I2S
    initialize_i2s(hardware_runtime.hardware());
    audio::set_providers(audio::open_codec, nullptr);
#if USE_BERRY_AUDIO
    audio::initialize_level();
    audio::initialize_spectrum();
#endif
    (void)i2s::configured_config(audio::command_config().input, platform::i2s::Direction::receive);
    (void)i2s::configured_config(
        audio::command_config().output, platform::i2s::Direction::transmit);
    (void)audio::initialize_store();
    commands::apply_audio_settings(audio::stored_settings());
#if USE_WIFI && (USE_AUDIO_MP3 || USE_AUDIO_AAC || USE_AUDIO_OPUS)
    audio::initialize_network();
#endif
#endif
#if USE_I2C
    initialize_i2c(hardware_runtime.hardware());
#endif
#if USE_CAMERA
    camera::initialize(hardware_runtime.hardware());
#endif
#if USE_ENERGY
    energy::service().select_hardware(hardware_runtime.hardware(), settings);
#endif
#if USE_RTC_CHIPS
    rtc::service().initialize(time_service, platform::monotonic_milliseconds());
#endif
#if USE_GPS
    gps::service().initialize(
        hardware_runtime.hardware(), time_service, platform::monotonic_milliseconds());
#endif
#if USE_SENSORS
    sensor_service().initialize(platform::monotonic_milliseconds(), settings.sensor_settings(),
        &hardware_runtime.hardware());
    sensor_service().set_presentation(settings.sensor_presentation());
    {
        std::uint8_t exact_history = 0U;
        (void)settings.option(177U, exact_history); // SetOption177: 5-minute history points
        (void)sensor_history().initialize(exact_history != 0U);
    }
#endif
#if USE_DISPLAY
    (void)initialize_display_from_filesystem(hardware_runtime.hardware());
#endif

#if USE_ESP_HOSTED
    hosted::initialize(hardware_runtime.hardware());
#endif
#if USE_WIFI
    network.initialize(platform::monotonic_milliseconds());
#endif
#if USE_ETHERNET
    // After Wi-Fi: it initialises the network stack and event loop the interface needs.
    ethernet::initialize(hardware_runtime.hardware(), network.hostname(), restart);
#endif
    // After Wi-Fi: the AmebaD controller needs the radio up and its mode settled.
#if USE_BLE
    std::uint8_t ble_enabled = 0U;
    (void)settings.option(115U, ble_enabled);
    // A boot loop skips the start for this boot only; SetOption115 1 still starts it.
    if (ble_enabled != 0U && !boot_guard().reduced()) {
        if (platform::ble_start())
            AddLog(LogLevel::info, "BLE: started");
        else
            AddLog(LogLevel::error, "BLE: start failed");
    }
#endif
#if USE_MATTER
    std::uint8_t matter_enabled = 0U;
    (void)settings.option(151U, matter_enabled);
    matter_module_enable(matter_enabled != 0U && !boot_guard().reduced());
    matter_module_bind_network(&network);
    matter_module_bind_power(&power);
#endif
#if USE_MQTT
#if USE_ENERGY_PROTECTION_POLICY
    energy::service().bind_protection_events(
        [](void* context, std::string_view suffix, std::string_view payload, bool result) {
            auto* owner = static_cast<MqttRuntime**>(context);
            return owner && *owner && (*owner)->service.queue_status_event(suffix, payload, result);
        },
        [](void* context) {
            auto* owner = static_cast<MqttRuntime**>(context);
            if (owner && *owner)
                (*owner)->service.request_sensor_telemetry();
        },
        &mqtt_runtime);
#endif
#if USE_ENERGY_POWER_DELTA
    energy::service().bind_power_delta(
        [](void* context, std::string_view suffix, std::string_view payload) {
            auto* owner = static_cast<MqttRuntime**>(context);
            return owner && *owner && (*owner)->service.queue_telemetry_event(suffix, payload);
        },
        [](void* context) {
            auto* owner = static_cast<MqttRuntime**>(context);
            if (owner && *owner)
                (*owner)->service.request_sensor_telemetry();
        },
        [](void*) { return power.operation_revision(); }, &mqtt_runtime);
#endif
#if USE_SENSORS
    sensor_service().set_event_sink(
        [](void* context, std::string_view payload) {
            auto* const owner = static_cast<MqttRuntime**>(context);
            return owner && *owner && (*owner)->service.queue_sensor_event(payload);
        },
        &mqtt_runtime);
#endif
    command_processor.set_mqtt_result_publisher(
        [](void* context, std::string_view suffix, std::string_view payload) {
            auto* const runtime_owner = static_cast<MqttRuntime**>(context);
            return runtime_owner != nullptr && *runtime_owner != nullptr &&
                   (*runtime_owner)->service.queue_command_result(suffix, payload);
        },
        &mqtt_runtime);
#if USE_DEEPSLEEP
    deep_sleep::bind_publisher(
        [](void* context, std::string_view payload) {
            auto* const runtime_owner = static_cast<MqttRuntime**>(context);
            return runtime_owner != nullptr && *runtime_owner != nullptr &&
                   (*runtime_owner)->service.queue_telemetry_event("DeepSleep", payload, true);
        },
        &mqtt_runtime);
#endif
    command_processor.set_mqtt_publisher(
        [](void* context, std::string_view topic, std::string_view payload, bool retained) {
            auto* const runtime_owner = static_cast<MqttRuntime**>(context);
            return runtime_owner != nullptr && *runtime_owner != nullptr &&
                   (*runtime_owner)->service.queue_publish(topic, payload, retained);
        },
        &mqtt_runtime);
#if USE_WEB
    web.set_mqtt_services(nullptr, nullptr);
    web.set_mqtt_test_allowed(false);
#endif
    SynchronizeMqttConfiguration();
#endif

#if USE_MESSAGE_ROUTING
#if USE_APP_MESSAGE_ROUTING
    PublishStateSnapshot(messages, platform::monotonic_milliseconds());
#elif USE_MQTT
    if (mqtt_runtime) {
        PublishStateSnapshot(mqtt_runtime->message_router(), platform::monotonic_milliseconds());
    }
#endif
#endif

    if (filesystem::is_mounted() && settings.read_only()) {
        AddLog(LogLevel::error,
            "CFG: unsupported or invalid record; settings writes locked until explicit Reset");
    }

#if USE_BERRY
    (void)berry.initialize(command_processor,
#if USE_PWM || USE_WS2812
        &light,
#else
        nullptr,
#endif
#if USE_MQTT
        &berry_mqtt,
#else
        nullptr,
#endif
#if USE_TIME
        &time_service,
#else
        nullptr,
#endif
        &hardware_runtime.hardware()
#if USE_TIMERS
            ,
        &timers
#endif
    );
    restart.set_before_restart_callback(save_application_state_before_restart, &berry);
#if USE_BERRY_ULP
    install_ulp_module(restart);
#endif
    std::uint8_t autoexec_disabled = 0U;
    (void)settings.option(153U, autoexec_disabled);
    autoexec.initialize(autoexec_disabled != 0U);
#endif

#if (USE_ENERGY || USE_SDCARD) && !USE_BERRY
    restart.set_before_restart_callback(save_application_state_before_restart, nullptr);
#endif

#if USE_TASMOTA_STATE
    commands::set_state_writer(WriteStateFields);
#endif
#if USE_TASMOTA_STATE && USE_MQTT
    commands::set_mqtt_status_reader([] {
        commands::MqttStatus status{};
        status.topic = network.mqtt_topic();
        if (mqtt_runtime != nullptr) {
            status.client = mqtt_runtime->service.client();
            status.connections = mqtt_runtime->client.stats().connections;
        }
        return status;
    });
#endif
#if NEWMOTA_DEEP_SLEEP
    restart.set_network_shutdown_callback(ShutdownNetworkForDeepSleep, nullptr);
#endif
    settings.mark_runtime_applied();

    Scheduler scheduler{platform::monotonic_milliseconds()};
    bool scheduler_announced = false;
    bool restart_power_flushed = false;
#if USE_MQTT && (USE_OTA_PULL || USE_WGET)
    bool mqtt_paused_for_transfer = false;
#endif

    while (true) {
        const auto now = platform::monotonic_milliseconds();
#if USE_I2S
#if USE_WIFI && (USE_AUDIO_MP3 || USE_AUDIO_AAC || USE_AUDIO_OPUS)
        audio::poll_network();
#endif
        audio::poll();
        i2s::poll();
#endif
#if defined(NEWMOTA_IMAGE_APPLICATION)
        boot_guard().poll(now);
#endif
#if USE_BERRY
        autoexec.poll();
#endif
#if USE_DISPLAY
        if (platform::display::info() != nullptr) {
            const auto result = platform::display::poll();
            if (result != platform::display::Result::success && !display_poll_fault_logged) {
                AddLog(LogLevel::error, "DSP: panel polling failed (%u)",
                    static_cast<unsigned int>(result));
                display_poll_fault_logged = true;
            }
        }
#endif
#if USE_PWM || USE_WS2812
        light.poll(now);
#endif
        console.poll(command_processor);
#if USE_SERIAL
        serial::poll();
#endif
#if USE_GPS
        gps::service().poll(now);
#endif
#if USE_CAMERA
        camera::poll(now);
#endif
#if USE_ADC
        adc::poll(now);
#endif
#if USE_MODBUS_RTU
        modbus::poll(platform::monotonic_milliseconds());
#endif
#if USE_WS2812
        led_strip::poll();
#endif
#if USE_WEB
        web.set_filesystem_access_allowed(
#if USE_OTA_PULL
            !url_update.active()
#else
            true
#endif
#if USE_RECOVERY_WEB || USE_BEKEN_UPDATE_WEB
            && !update_web.active()
#endif
#if USE_WGET
            && !wget.active()
#endif
        );
#if USE_MQTT
        web.set_mqtt_test_allowed(network.online() && mqtt_runtime_enabled
#if USE_OTA_PULL
                                  && !url_update.active()
#endif
#if USE_WGET
                                  && !wget.active()
#endif
#if USE_BEKEN_UPDATE_WEB
                                  && !update_web.active()
#endif
        );
#endif
        web.poll(now, network.available());
#endif
#if USE_RECOVERY_WEB || USE_BEKEN_UPDATE_WEB
        update_web.poll(now);
#endif
#if USE_RECOVERY_WEB
        recovery_web.poll(now, network.available());
#endif
#if USE_OTA_PULL
        url_update.poll(now, network.available());
#endif
#if USE_WGET
        wget.poll(now, network.online(), restart.pending());
#endif
#if USE_ESP_HOSTED && defined(NEWMOTA_IMAGE_APPLICATION)
        hosted::poll(now);
#endif
#if USE_PING
        ping.poll(now, network.online(), restart.pending());
        ping.deliver(WriteConsole, nullptr, PublishPingResult, &messages);
#endif
#if USE_MQTT && (USE_OTA_PULL || USE_WGET)
        const bool outbound_transfer_active = false
#if USE_OTA_PULL
                                              || url_update.active()
#endif
#if USE_WGET
                                              || wget.owns_stream()
#endif
            ;
        if (outbound_transfer_active && !mqtt_paused_for_transfer) {
            if (mqtt_runtime) {
                mqtt_runtime->client.disconnect();
                mqtt_runtime->service.reset_connection();
            }
            mqtt_paused_for_transfer = true;
        } else if (!outbound_transfer_active && mqtt_paused_for_transfer) {
            mqtt_paused_for_transfer = false;
        }
#endif
        if (restart.pending() && !restart_power_flushed) {
            if (settings.flush_power_state(power.state_mask()) == SettingsUpdateResult::failed) {
                AddLog(LogLevel::error, "CFG: power state save before restart failed");
            }
            restart_power_flushed = true;
        }
#if USE_DEFER
        // A restart drops waiting Defer commands, as it would lose them anyway.
        if (restart.pending()) {
            defer::clear();
        } else {
            defer::poll(command_processor, now);
        }
#endif
        restart.poll(now);
        const auto ticks = scheduler.poll(now);
#ifndef NEWMOTA_IMAGE_SAFEBOOT
        PollInputs(now);
#endif
#if USE_TUYA_MCU
        tuya::poll(now, TuyaLinkState());
#endif
#if USE_STATUS_LEDS
        if (has_tick(ticks, ScheduledTick::every_250_ms)) {
            power.leds().tick(CurrentLedInputs(), CurrentLedPolicy());
        }
#endif
        if (has_tick(ticks, ScheduledTick::every_100_ms)) {
            power.poll_timers(now);
#if USE_MESSAGE_ROUTING
#if USE_APP_MESSAGE_ROUTING
            PublishStateSnapshot(messages, now);
#elif USE_MQTT
            if (mqtt_runtime) {
                PublishStateSnapshot(mqtt_runtime->message_router(), now);
            }
#endif
#endif
        }
#if USE_WIFI
        if (has_tick(ticks, ScheduledTick::every_250_ms)) {
            network.poll(now);
#if USE_SLEEP
            ApplyWifiPowerSave(settings);
#endif
#if USE_WIFI_POWER
            wifi_tx_power::apply(settings);
#endif
#if USE_APP_MESSAGE_ROUTING
            PublishWifiScanAnnouncements(&messages);
#elif USE_MQTT
            PublishWifiScanAnnouncements(mqtt_runtime ? &mqtt_runtime->message_router() : nullptr);
#else
            PublishWifiScanAnnouncements(nullptr);
#endif
        }
#endif
#if USE_MQTT
        if (has_tick(ticks, ScheduledTick::every_50_ms)) {
            const bool outbound_transfer_active = false
#if USE_OTA_PULL
                                                  || url_update.active()
#endif
#if USE_WGET
                                                  || wget.owns_stream()
#endif
                ;
            if (!outbound_transfer_active)
                SynchronizeMqttConfiguration();
#if USE_WEB
            web.set_mqtt_test_allowed(
                !outbound_transfer_active && network.online() && mqtt_runtime_enabled
#if USE_WGET
                && !wget.active()
#endif
            );
#endif
            if (mqtt_runtime_enabled && !outbound_transfer_active) {
                auto& runtime = *mqtt_runtime;
                if (network.online()) {
                    runtime.client.poll(now);
                    if (!runtime.client.connection_test_active()) {
                        runtime.service.poll(now);
#if USE_TASMOTA_STATE
                        runtime.commands.poll();
#endif
#if USE_HA_DISCOVERY
#if USE_HARDWARE_COMMANDS
                        const auto model = SelectedModuleName(settings.hardware_configuration());
#else
                        constexpr std::string_view model = "Newmota";
#endif
                        ha_discovery::poll(runtime.service, settings, power, model, now);
#endif
                    }
                } else {
                    runtime.client.disconnect();
                    runtime.service.poll(now);
#if USE_TASMOTA_STATE
                    runtime.commands.poll();
#endif
                }
            }
        }
#endif
#if USE_TIME
        if (has_tick(ticks, ScheduledTick::every_50_ms)) {
            // Commands earlier in this iteration may have accepted a newer clock sample.
            const auto time_now = platform::monotonic_milliseconds();
#if USE_NTP
            time_service.poll(time_now, network.online());
#else
            time_service.poll(time_now, false);
#endif
        }
#endif
#if USE_NTP_SERVER
        if (has_tick(ticks, ScheduledTick::every_50_ms)) {
            const auto wifi = platform::wifi_snapshot();
            platform::ntp_server_poll(
                time_service.ntp_server_clock(platform::monotonic_milliseconds()),
                network.online() || wifi.access_point == platform::AccessPointState::active);
        }
#endif
#if USE_TIMERS
        timers.poll(now);
#endif
#if USE_BLE
        ble::poll_advertisements();
#endif
#if USE_BERRY
#if USE_BERRY_DRIVERS
#if USE_WIFI
        const bool berry_online = network.online();
#else
        constexpr bool berry_online = false;
#endif
        observe_berry_drivers(static_cast<std::uint8_t>(ticks), power.state_mask(), berry_online);
#if USE_I2S
        const auto i2s_activity = i2s::activity();
        observe_berry_audio(
            i2s_activity.tx, i2s_activity.tx_rate, i2s_activity.rx, i2s_activity.rx_rate);
#endif
#endif
        berry.poll(now, has_tick(ticks, ScheduledTick::every_50_ms));
#endif
#if USE_EDGE_CAPTURE
        edges::poll();
#endif
#if USE_ENERGY
        energy::service().poll(now);
#endif
        if (has_tick(ticks, ScheduledTick::every_second)) {
            uptime_seconds(); // keeps counting across the millisecond clock wrap
#if USE_WEB_CONSOLE
            console_history::maintain(now);
#endif
#if USE_GLOBAL_TEMPERATURE
            global_temperature().poll(now);
#endif
#if USE_SENSORS
            sensor_service().poll(now);
            sensor_history().poll(now);
#endif
#if USE_GLOBAL_TEMPERATURE_COMMANDS
            refresh_global_temperature_selection(settings, now);
#endif
#if USE_ENERGY_OVERTEMP
            energy::service().poll_overtemperature(now);
#endif
#if USE_SENSORS || USE_ENERGY || USE_SWITCHES
#if USE_MESSAGE_ROUTING
#if USE_APP_MESSAGE_ROUTING
            PublishSensorSnapshot(messages, now);
#elif USE_MQTT
            if (mqtt_runtime) {
                PublishSensorSnapshot(mqtt_runtime->message_router(), now);
            }
#endif
#endif
#endif
            if (settings.poll_power_state(power.state_mask()) == SettingsUpdateResult::failed) {
                AddLog(LogLevel::error, "CFG: periodic power state save failed");
            }
#if USE_DEEPSLEEP
            deep_sleep::poll_second(settings, time_service, restart,
#if USE_WIFI
                network.online(),
#else
                false,
#endif
                now);
#endif
            if (!scheduler_announced) {
                AddLog(LogLevel::info, "RTM: scheduler active");
                scheduler_announced = true;
            }
        }
        platform::sleep_for_milliseconds(kIdleSleepMilliseconds);
#if USE_SLEEP
        // Tasmota's dynamic sleep: an idle device makes one pass per Sleep milliseconds. Work
        // that needs the 1 ms pass keeps it, and console or web input ends the wait at once.
#if USE_I2S || USE_EDGE_CAPTURE || USE_DISPLAY
        bool busy = true; // these drivers keep their own timing in every pass
#else
        bool busy = restart.pending();
#endif
#if USE_PWM || USE_WS2812
        busy = busy || light.transitioning() || light.addressable();
#endif
#if USE_SERIAL
        busy = busy || serial::any_active(); // receive rings fill within one Sleep period
#endif
#if USE_BLE
        busy = busy || platform::ble_ready(); // one advertisement slot
#endif
#if USE_OTA_PULL
        busy = busy || url_update.active();
#endif
#if USE_WGET
        busy = busy || wget.active();
#endif
#if USE_BEKEN_UPDATE_WEB
        busy = busy || update_web.active();
#endif
#if USE_WEB
        busy = busy || web.filesystem_upload_active();
#endif
        // SetOption60 (normal sleep) waits the full period after the pass instead of
        // finishing the period that started with it.
        std::uint8_t normal_sleep = 0U;
        (void)settings.option(60U, normal_sleep);
        const auto pass_end = platform::monotonic_milliseconds();
        loop_sleep::record_pass(pass_end - now, settings.active_sleep(), normal_sleep != 0U);
        const std::uint32_t wait_until =
            (normal_sleep != 0U ? pass_end : now) + (busy ? 0U : settings.active_sleep());
        while (static_cast<std::int32_t>(wait_until - platform::monotonic_milliseconds()) > 0) {
#ifndef NEWMOTA_IMAGE_SAFEBOOT
            PollInputs(platform::monotonic_milliseconds());
#endif
            bool work = console.poll(command_processor);
#if USE_WEB
            work = web.poll(platform::monotonic_milliseconds(), network.available()) || work;
#endif
            if (work)
                break;
            platform::sleep_for_milliseconds(kIdleSleepMilliseconds);
        }
#endif
    }
}

} // namespace newmota
