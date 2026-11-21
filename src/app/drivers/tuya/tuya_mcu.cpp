/*
  tuya_mcu.cpp - TuyaMCU serial driver for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Reference: Tasmota xdrv_16_tuyamcu_v2.ino, Copyright (C) 2021 Federico Leoni, digiblur,
  Joel Stein and Theo Arends, GPL version 3 or later.
*/

/** @file tuya_mcu.cpp
 * @brief Port of Tasmota's TuyaMCU v2: framing, startup state machine, data point store,
 * relays, switches, the light, sensors, enums, TuyaReceived and the TuyaMCU, TuyaSend,
 * TuyaEnum, TuyaEnumList, TuyaTempSetRes, TuyaRGB, TuyaDimDelay and DimmerRange commands.
 */
#include "drivers/tuya/tuya_mcu.h"

#include "core/hardware/hardware.h"
#include "core/system/restart.h"
#include "core/system/uptime.h"
#include "drivers/serial/serial_service.h"
#include "gpio_ids.h"
#include "newmota/log.h"
#include "newmota/platform/platform.h"
#include "services/power/power_service.h"
#include "services/settings/settings.h"
#include "support/number.h"
#include "support/text.h"
#if USE_PWM
#include "services/light/light_api.h"
#include "services/light/light_color.h"
#include "services/light/light_service.h"
#endif
#if USE_TIME
#include "services/time/time_service.h"
#endif

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <utility>

namespace newmota::tuya {
namespace {

constexpr std::uint8_t kCmdHeartbeat = 0x00;
constexpr std::uint8_t kCmdQueryProduct = 0x01;
constexpr std::uint8_t kCmdMcuConf = 0x02;
constexpr std::uint8_t kCmdWifiState = 0x03;
constexpr std::uint8_t kCmdWifiReset = 0x04;
constexpr std::uint8_t kCmdWifiSelect = 0x05;
constexpr std::uint8_t kCmdSetDp = 0x06;
constexpr std::uint8_t kCmdState = 0x07;
constexpr std::uint8_t kCmdQueryState = 0x08;
constexpr std::uint8_t kCmdUpgradePackage = 0x0B;
constexpr std::uint8_t kCmdSetTime = 0x1C;
constexpr std::uint8_t kCmdNewFeatures = 0x37;
constexpr std::uint8_t kLowPowerCmdWifiState = 0x02;
constexpr std::uint8_t kLowPowerCmdState = 0x05;

constexpr std::uint8_t kTypeRaw = 0x00;
constexpr std::uint8_t kTypeBool = 0x01;
constexpr std::uint8_t kTypeValue = 0x02;
constexpr std::uint8_t kTypeString = 0x03;
constexpr std::uint8_t kTypeEnum = 0x04;

constexpr std::size_t kMaxStringSize = 16;
constexpr std::size_t kBufferSize = 256;
constexpr std::size_t kMaxStoredDps = 10;
constexpr std::uint32_t kByteTimeoutMs = 500;
constexpr std::uint8_t kHeartbeatIntervalSeconds = 15;

// Tasmota's TuyaSupportedFunctions in use here.
constexpr std::uint8_t kFnSwitch1 = 1;
constexpr std::uint8_t kFnSwitch4 = 4;
constexpr std::uint8_t kFnRelay1 = 11;
constexpr std::uint8_t kFnRelay8 = 18;
constexpr std::uint8_t kFnDimmer = 21;
constexpr std::uint8_t kFnDimmer2 = 22;
constexpr std::uint8_t kFnCt = 23;
constexpr std::uint8_t kFnRgb = 24;
constexpr std::uint8_t kFnWhite = 25;
constexpr std::uint8_t kFnModeSet = 26;
constexpr std::uint8_t kFnReport1 = 27;
constexpr std::uint8_t kFnReport2 = 28;
constexpr std::uint8_t kFnPower = 31;
constexpr std::uint8_t kFnCurrent = 32;
constexpr std::uint8_t kFnVoltage = 33;
constexpr std::uint8_t kFnPowerCombined = 36;
constexpr std::uint8_t kFnPowerTotal = 37;
constexpr std::uint8_t kFnRelay1Inv = 41;
constexpr std::uint8_t kFnRelay8Inv = 48;
constexpr std::uint8_t kFnLowPowerMode = 51;
constexpr std::uint8_t kFnEnum1 = 61;
constexpr std::uint8_t kFnEnum4 = 64;
constexpr std::uint8_t kFnTemp = 71;
constexpr std::uint8_t kFnTempSet = 72;
constexpr std::uint8_t kFnHum = 73;
constexpr std::uint8_t kFnHumSet = 74;
constexpr std::uint8_t kFnLx = 75;
constexpr std::uint8_t kFnPm25 = 80;
constexpr std::uint8_t kFnTimer1 = 81;
constexpr std::uint8_t kFnTimer4 = 84;
constexpr std::uint8_t kFnMotorDir = 97;
constexpr std::uint8_t kFnDummy = 99;
constexpr std::size_t kEnumCount = kFnEnum4 - kFnEnum1 + 1U;
#if USE_TUYA_MCU_SENSORS
constexpr std::size_t kSensorCount = kFnTimer4 - kFnTemp + 1U;
#endif

// Tasmota's CT range without SetOption82, and its default TuyaDimDelay.
constexpr std::uint16_t kCtMin = 153;
constexpr std::uint16_t kCtMax = 500;
constexpr int kDefaultDimDelayMs = 2000;

// Commands not published as TuyaReceived with SetOption137 (TuyaExcludeCMDsFromMQTT).
constexpr std::array<std::uint8_t, 4> kExcludedFromMqtt{
    kCmdHeartbeat, kCmdWifiState, kCmdSetTime, kCmdUpgradePackage};

enum class Startup : std::uint8_t {
    init,
    wait_ack_init,
    product,
    wait_ack_product,
    wait_optional_new_features,
    conf,
    wait_ack_conf,
    wifi_state,
    wait_ack_wifi,
    query_state,
    wait_ack_query,
    send_cmd,
    wait_ack_cmd,
    send_heartbeat,
    wait_ack_heartbeat,
};

/** @brief Last value the MCU reported and the value to send, so a value is sent only when it
 * differs: a second `off` can crash some dimmers.
 */
struct StoredDp final {
    std::uint8_t id{};
    std::uint8_t type{};
    std::uint8_t rxed_length{};
    std::uint8_t desired_length{};
    std::array<std::uint8_t, kMaxStringSize> rxed{};
    std::array<std::uint8_t, kMaxStringSize> desired{};
    bool to_set{};
};

struct Driver final {
    Settings* settings{};
    PowerService* power{};
#if USE_TIME
    TimeService* time{};
#endif
    Publisher publisher{};
    void* publisher_context{};
    serial::Handle handle{};
    bool active{};
    bool low_power_mode{};
    bool send_success_next_second{};
    std::uint8_t reported_relay{}; ///< Relay whose MCU report Power applies now, 0 for none
    bool reported_on{};
    bool in_state_machine{};
    bool in_report{}; ///< An MCU report is being applied
    std::uint32_t relays{};
    std::uint32_t switches{};
#if USE_PWM
    LightService* light{};
    LightLayout layout{};
    std::array<std::uint8_t, 2> light_relays{}; ///< Relay switching each light endpoint, 0 none
    std::array<std::uint16_t, 2> levels{};      ///< Tasmota Levels: last MCU dimmer, CT or white
    std::array<std::uint16_t, 3> hsb{};         ///< Last colour sent or reported: hue, sat %, bri %
    std::array<char, 7> rgb{"000000"};          ///< Last RRGGBB sent or reported
    bool mode_color{};                          ///< Tasmota ModeSet: the MCU shows colour
    std::array<int, 2> dim_delay_ms{};          ///< Time left before dim values may be sent
    int default_dim_delay_ms{kDefaultDimDelayMs};
    std::uint8_t dim_enable{}; ///< Dim values allowed: the endpoint's relay was reported on
#endif
    std::array<std::uint8_t, kEnumCount> enum_state{}; ///< Tasmota EnumState: last sent or reported
#if USE_TUYA_MCU_SENSORS
    std::array<std::int32_t, kSensorCount> sensors{}; ///< Last reported value of fnId 71..84
    std::uint16_t sensors_reported{};                 ///< Bit per fnId: reported since start
#endif
#if USE_TUYA_MCU_ENERGY
    EnergyReading energy{};
#endif

    std::array<StoredDp, kMaxStoredDps> dps{};
    std::size_t dp_count{};

    std::array<std::uint8_t, kBufferSize> buffer{};
    std::size_t byte_counter{};
    std::uint8_t cmd_status{};
    std::uint8_t cmd_checksum{};
    std::uint16_t data_length{};
    std::uint32_t last_byte_time{};

    std::array<std::uint8_t, 320> tx{};
    std::size_t tx_size{};
    std::size_t tx_sent{};

    std::uint8_t wifi_state{0xFEU}; ///< Tasmota's -2: unknown, resent on the next pass
    LinkState link{LinkState::offline};
    std::uint8_t heartbeat_timer{};
    bool send_heartbeat{};
    std::uint8_t send_time{};

    int timeout_ms{};
    Startup startup_state{Startup::init};
    Startup timeout_state{Startup::init};
    std::uint8_t expected_response{};

    bool suspend_topic{};
    std::uint32_t topic_resume_at{};
    std::uint32_t errors{};
    std::uint32_t logged_errors{};
    std::uint32_t next_tick{};
    std::uint32_t next_second{};
    std::uint32_t sends{};
    std::uint32_t receives{};
};

Driver driver;

/** @brief Reports whether a debug line would reach the serial or web log. */
[[nodiscard]] bool debug_logging() {
    constexpr auto debug = static_cast<std::uint8_t>(LogLevel::debug);
    return log_detail::serial_log_level >= debug || log_detail::web_log_level >= debug;
}

/** @brief Reports whether a wrapping millisecond deadline has passed. */
[[nodiscard]] bool reached(const std::uint32_t now, const std::uint32_t deadline) {
    return static_cast<std::int32_t>(now - deadline) >= 0;
}

[[nodiscard]] std::uint32_t read32(const std::uint8_t* const data) {
    return static_cast<std::uint32_t>(data[0]) << 24U | static_cast<std::uint32_t>(data[1]) << 16U |
           static_cast<std::uint32_t>(data[2]) << 8U | data[3];
}

#if USE_PWM
/** @brief Tasmota's changeUIntScale: maps @p value from one range to another, rounding, and
 * reverses the result when @p to_min is above @p to_max.
 */
[[nodiscard]] std::uint32_t change_scale(std::uint32_t value, const std::uint32_t from_min,
    const std::uint32_t from_max, std::uint32_t to_min, std::uint32_t to_max) {
    if (from_min >= from_max)
        return to_min > to_max ? to_max : to_min;
    value = value > from_max ? from_max : value < from_min ? from_min : value;
    if (to_min > to_max) {
        value = from_max - value + from_min;
        std::swap(to_min, to_max);
    }
    if (value == from_min)
        return to_min;
    if (value == from_max)
        return to_max;
    const std::uint64_t offset = value - from_min;
    const std::uint64_t from = from_max - from_min;
    const std::uint64_t to = to_max - to_min;
    if (to > from)
        return static_cast<std::uint32_t>((offset * to * 2U / from + 1U) / 2U + to_min);
    return static_cast<std::uint32_t>((offset * 2U + 1U) * (to + 1U) / ((from + 1U) * 2U) + to_min);
}
#endif

/** @brief Returns the function of a data point, 0 if none, as TuyaGetFuncId. */
[[nodiscard]] std::uint8_t function_of(const Settings::TuyaSettings& tuya, const std::uint8_t dp) {
    for (const auto& entry : tuya.functions)
        if (entry.dp == dp)
            return entry.function;
    return 0U;
}

/** @brief Returns the data point of a function, 0 if none, as TuyaGetDpId. */
[[nodiscard]] std::uint8_t dp_of(const Settings::TuyaSettings& tuya, const std::uint8_t function) {
    for (const auto& entry : tuya.functions)
        if (entry.function == function)
            return entry.dp;
    return 0U;
}

[[nodiscard]] std::uint8_t dp_of(const std::uint8_t function) {
    return dp_of(driver.settings->tuya_settings(), function);
}

/** @brief Tasmota's TuyaFuncIdValid. */
[[nodiscard]] constexpr bool valid_function(const std::uint8_t fn) {
    return (fn >= kFnSwitch1 && fn <= kFnSwitch4) || (fn >= kFnRelay1 && fn <= kFnRelay8) ||
           (fn >= kFnDimmer && fn <= kFnReport2) || (fn >= kFnPower && fn <= kFnPowerTotal) ||
           (fn >= kFnRelay1Inv && fn <= kFnRelay8Inv) || (fn >= kFnEnum1 && fn <= kFnEnum4) ||
           (fn >= kFnMotorDir && fn <= kFnDummy) || fn == kFnLowPowerMode ||
           (fn >= kFnTemp && fn <= kFnHumSet) || (fn >= kFnLx && fn <= kFnPm25) ||
           (fn >= kFnTimer1 && fn <= kFnTimer4);
}

/** @brief Tasmota's TuyaAddMcuFunc: adds or updates a pair, or deletes it with a zero. */
void add_function(Settings::TuyaSettings& tuya, const std::uint8_t fn, const std::uint8_t dp) {
    auto& map = tuya.functions;
    if (fn == 0U || dp == 0U) {
        for (auto& entry : map) {
            if ((dp > 0U && entry.dp == dp) || (fn > 0U && entry.function == fn)) {
                entry = {};
                break;
            }
        }
        return;
    }
    bool added = false;
    for (auto& entry : map) {
        if (entry.dp == dp || entry.dp == 0U || entry.function == fn || entry.function == 0U) {
            if (!added) {
                entry = {fn, dp};
                added = true;
            } else if (entry.dp == dp || entry.function == fn) {
                entry = {};
            }
        }
    }
}

/** @brief Sends whatever is queued without waiting. */
void flush_tx() {
    while (driver.tx_sent < driver.tx_size) {
        const auto written = serial::write_some(
            driver.handle, driver.tx.data() + driver.tx_sent, driver.tx_size - driver.tx_sent);
        if (written.result != serial::Result::success || written.count == 0U)
            return;
        driver.tx_sent += written.count;
    }
    driver.tx_size = driver.tx_sent = 0U;
}

/** @brief Frames and queues one command, as TuyaSendCmd. */
void send_command(const std::uint8_t cmd, const std::uint8_t* const payload = nullptr,
    const std::uint16_t length = 0U) {
    if (driver.tx_size + 7U + length > driver.tx.size()) {
        AddLog(LogLevel::error, "TYA: Send queue full, cmd %u dropped", unsigned(cmd));
        ++driver.errors;
        return;
    }
    auto* const frame = driver.tx.data() + driver.tx_size;
    frame[0] = 0x55U;
    frame[1] = 0xAAU;
    frame[2] = 0x00U; // version 00: to the MCU
    frame[3] = cmd;
    frame[4] = static_cast<std::uint8_t>(length >> 8U);
    frame[5] = static_cast<std::uint8_t>(length & 0xFFU);
    std::uint8_t checksum = static_cast<std::uint8_t>(0xFFU + cmd + (length >> 8U) + length);
    for (std::uint16_t i = 0U; i < length; ++i) {
        frame[6U + i] = payload[i];
        checksum = static_cast<std::uint8_t>(checksum + payload[i]);
    }
    frame[6U + length] = checksum;
    driver.tx_size += 7U + length;
    if (debug_logging()) {
        char hex[2U * (7U + 64U) + 1U]{};
        const std::size_t shown = 7U + (length < 64U ? length : 64U);
        for (std::size_t i = 0U; i < shown; ++i)
            std::snprintf(
                hex + 2U * i, 3U, "%02x", unsigned(i < 6U + length ? frame[i] : checksum));
        AddLog(LogLevel::debug, "TYA: T:>%s", hex);
    }
    ++driver.sends;
    flush_tx();
}

/** @brief Frames one data point as TuyaSendState. */
void send_state(const std::uint8_t id, const std::uint8_t type, const std::uint8_t* const value,
    const std::size_t length) {
    std::array<std::uint8_t, 8U + kMaxStringSize> payload{};
    std::uint16_t size = 4U;
    payload[0] = id;
    payload[1] = type;
    switch (type) {
    case kTypeBool:
    case kTypeEnum:
        payload[3] = 0x01U;
        payload[4] = value[0];
        size += 1U;
        break;
    case kTypeValue:
        payload[3] = 0x04U;
        for (std::size_t i = 0U; i < 4U; ++i)
            payload[4U + i] = value[i];
        size += 4U;
        break;
    default:
        if (length > kMaxStringSize) {
            AddLog(LogLevel::error, "TYA: SendState: DP %u value over len (%u > %u)", unsigned(id),
                unsigned(length), unsigned(kMaxStringSize));
            return;
        }
        payload[2] = static_cast<std::uint8_t>(length >> 8U);
        payload[3] = static_cast<std::uint8_t>(length & 0xFFU);
        for (std::size_t i = 0U; i < length; ++i)
            payload[size++] = value[i];
        break;
    }
    send_command(kCmdSetDp, payload.data(), size);
}

/** @brief Asks for a data point value; the state machine sends it once it differs from the
 * MCU's last report, as TuyaPostState.
 */
bool post_state(const std::uint8_t id, const std::uint8_t type, const std::uint8_t* const value,
    const std::size_t length) {
    if (length > kMaxStringSize) {
        AddLog(LogLevel::error, "TYA: DP %u value over len (%u > %u)", unsigned(id),
            unsigned(length), unsigned(kMaxStringSize));
        return false;
    }
    for (std::size_t i = 0U; i < driver.dp_count; ++i) {
        auto& dp = driver.dps[i];
        if (dp.id != id)
            continue;
        if (dp.type != type) {
            AddLog(LogLevel::error, "TYA: set of dpid %u ignored - type %u != requred %u",
                unsigned(id), unsigned(type), unsigned(dp.type));
            return false;
        }
        std::memcpy(dp.desired.data(), value, length);
        dp.desired_length = static_cast<std::uint8_t>(length);
        dp.to_set = true;
        AddLog(LogLevel::debug, "TYA: DP%u des v set (0x%x,%u)", unsigned(id),
            unsigned(dp.desired[0]), unsigned(length));
        return true;
    }
    StoredDp* slot = driver.dp_count < kMaxStoredDps ? &driver.dps[driver.dp_count++] : nullptr;
    if (slot == nullptr) {
        // Tasmota drops the value once ten data points were seen. Newmota replaces one with
        // nothing to send, preferring data points without a mapped function, so reports of
        // other data points cannot lock out a relay.
        const auto& tuya = driver.settings->tuya_settings();
        for (std::size_t pass = 0U; pass < 2U && slot == nullptr; ++pass)
            for (auto& candidate : driver.dps)
                if (!candidate.to_set && (pass == 1U || function_of(tuya, candidate.id) == 0U)) {
                    slot = &candidate;
                    break;
                }
    }
    if (slot == nullptr) {
        AddLog(LogLevel::error,
            "TYA: Max Stored DPs exceeded at set of unknown dpid %u ignored - num DP:%u",
            unsigned(id), unsigned(driver.dp_count));
        return false;
    }
    auto& dp = *slot;
    dp = {};
    dp.id = id;
    dp.type = type;
    std::memcpy(dp.desired.data(), value, length);
    dp.desired_length = static_cast<std::uint8_t>(length);
    dp.to_set = true;
    AddLog(LogLevel::debug, "TYA: NEW DP %u desiredvalue set (0x%x len %u)", unsigned(id),
        unsigned(dp.desired[0]), unsigned(length));
    return true;
}

bool post_bool(const std::uint8_t id, const bool value) {
    const std::uint8_t byte = value ? 1U : 0U;
    return post_state(id, kTypeBool, &byte, 1U);
}

void post_value(const std::uint8_t id, const std::uint32_t value) {
    const std::array<std::uint8_t, 4> bytes{static_cast<std::uint8_t>(value >> 24U),
        static_cast<std::uint8_t>(value >> 16U), static_cast<std::uint8_t>(value >> 8U),
        static_cast<std::uint8_t>(value)};
    (void)post_state(id, kTypeValue, bytes.data(), bytes.size());
}

void post_enum(const std::uint8_t id, const std::uint32_t value) {
    const auto byte = static_cast<std::uint8_t>(value);
    (void)post_state(id, kTypeEnum, &byte, 1U);
}

/** @brief Converts pairs of hex digits, as Tasmota's convertHexStringtoBytes. */
std::size_t hex_bytes(const char* const text, std::uint8_t* const output, const std::size_t count) {
    for (std::size_t i = 0U; i < count; ++i) {
        const char pair[3]{text[2U * i], text[2U * i + 1U], '\0'};
        output[i] = static_cast<std::uint8_t>(std::strtol(pair, nullptr, 16));
    }
    return count;
}

void post_hex(const std::uint8_t id, const std::uint8_t type, const char* const text) {
    const std::size_t length = std::strlen(text) / 2U;
    std::array<std::uint8_t, 128> bytes{};
    if (length > bytes.size()) {
        AddLog(LogLevel::error, "TYA: DP %u value over len (%u > %u)", unsigned(id),
            unsigned(length), unsigned(kMaxStringSize));
        return;
    }
    (void)post_state(id, type, bytes.data(), hex_bytes(text, bytes.data(), length));
}

#if USE_TIME
/** @brief Sends local time, as TuyaSetTime; nothing before the clock is valid. */
void send_time() {
    if (driver.time == nullptr)
        return;
    const auto snapshot = driver.time->snapshot(platform::monotonic_milliseconds());
    if (!snapshot.valid)
        return;
    const auto civil = TimeService::civil_time(snapshot.local_seconds);
    const std::array<std::uint8_t, 8> payload{0x01U, static_cast<std::uint8_t>(civil.year % 100U),
        civil.month, civil.day, civil.hour, civil.minute, civil.second,
        // Tuya counts Monday as 1 and Sunday as 7.
        static_cast<std::uint8_t>(civil.day_of_week == 1U ? 7U : civil.day_of_week - 1U)};
    send_command(kCmdSetTime, payload.data(), payload.size());
}
#endif

#if USE_PWM
/** @brief Reports whether a light value must wait, as Tasmota's dimDelay_ms and dimCmdEnable:
 * shortly after the MCU reported its relay or a dim value, and while its relay is off.
 */
[[nodiscard]] bool dim_held(const std::uint8_t id) {
    const auto fn = function_of(driver.settings->tuya_settings(), id);
    if (fn < kFnDimmer || fn > kFnWhite)
        return false;
    const std::size_t index = fn == kFnDimmer2 ? 1U : 0U;
    return driver.dim_delay_ms[index] > 0 || (driver.dim_enable & (1U << index)) == 0U;
}
#endif

void wait_for(const int timeout_ms, const Startup timeout_state, const Startup state) {
    driver.timeout_ms = timeout_ms;
    driver.timeout_state = timeout_state;
    driver.startup_state = state;
}

/** @brief Tasmota's Tuya_statemachine: called every 100 ms with cmd -1, and with each received
 * command to recognise acknowledgements.
 */
void state_machine(const int cmd = -1, const std::size_t length = 0U,
    const std::uint8_t* const payload = nullptr) {
    driver.in_state_machine = true;
    const auto state = driver.startup_state;
    switch (driver.startup_state) {
    case Startup::init:
        if (cmd >= 0)
            break;
        // Low-power MCUs know no heartbeat: start with the product query, as Tasmota v1.
        if (driver.low_power_mode) {
            driver.startup_state = Startup::product;
            break;
        }
        send_command(kCmdHeartbeat);
        wait_for(3000, Startup::init, Startup::wait_ack_init);
        break;
    case Startup::wait_ack_init:
        if (cmd == kCmdHeartbeat)
            wait_for(0, Startup::init, Startup::product);
        break;
    case Startup::product:
        if (cmd >= 0)
            break;
        send_command(kCmdQueryProduct);
        wait_for(1000, Startup::init, Startup::wait_ack_product);
        break;
    case Startup::wait_ack_product:
        // The MCU may announce new features within 300 ms of its product answer. A low-power
        // MCU has no configuration, Wi-Fi state or state query step: it reports on its own.
        if (cmd == kCmdQueryProduct && driver.low_power_mode)
            wait_for(0, Startup::send_cmd, Startup::send_cmd);
        else if (cmd == kCmdQueryProduct)
            wait_for(300, Startup::conf, Startup::wait_optional_new_features);
        break;
    case Startup::wait_optional_new_features:
        if (cmd == kCmdNewFeatures)
            wait_for(0, Startup::conf, Startup::conf);
        break;
    case Startup::conf:
        if (cmd >= 0)
            break;
        send_command(kCmdMcuConf);
        wait_for(1000, Startup::init, Startup::wait_ack_conf);
        break;
    case Startup::wait_ack_conf:
        if (cmd == kCmdMcuConf)
            wait_for(0, Startup::init, length > 0U ? Startup::query_state : Startup::wifi_state);
        break;
    case Startup::wifi_state: {
        if (cmd >= 0)
            break;
        const std::uint8_t connected = 0x04U;
        send_command(kCmdWifiState, &connected, 1U);
        wait_for(1000, Startup::init, Startup::wait_ack_wifi);
        break;
    }
    case Startup::wait_ack_wifi:
        if (cmd == kCmdWifiState)
            wait_for(0, Startup::init, Startup::query_state);
        break;
    case Startup::query_state:
        if (cmd >= 0)
            break;
        send_command(kCmdQueryState);
        wait_for(1000, Startup::init, Startup::wait_ack_query);
        break;
    case Startup::wait_ack_query:
        // Wait 500 ms after the last state report before running.
        if (cmd == kCmdState)
            wait_for(500, Startup::send_cmd, Startup::wait_ack_query);
        break;
    case Startup::send_cmd: {
        if (cmd >= 0)
            break;
#if USE_TIME
        if ((driver.send_time & 1U) != 0U) {
            send_time();
            driver.send_time &= 0xFEU;
            driver.expected_response = kCmdSetTime;
            wait_for(200, Startup::send_cmd, Startup::wait_ack_cmd);
            break;
        }
#endif
        const auto link = static_cast<std::uint8_t>(driver.link);
        if (driver.wifi_state != link) {
            driver.wifi_state = link;
            AddLog(LogLevel::debug, "TYA: Set WiFi LED %u", unsigned(link));
            driver.expected_response =
                driver.low_power_mode ? kLowPowerCmdWifiState : kCmdWifiState;
            send_command(driver.expected_response, &driver.wifi_state, 1U);
            wait_for(300, Startup::send_cmd, Startup::wait_ack_cmd);
            break;
        }
        if (driver.low_power_mode && driver.send_success_next_second) {
            const std::uint8_t success = 1U;
            send_command(kLowPowerCmdState, &success, 1U);
            driver.expected_response = kLowPowerCmdState;
            wait_for(300, Startup::send_cmd, Startup::wait_ack_cmd);
            driver.send_success_next_second = false;
            break;
        }
        bool sent = false;
        // A mode change goes first, so the colour or white value after it meets the right mode.
        const auto mode_dp = dp_of(kFnModeSet);
        for (std::size_t pass = mode_dp != 0U ? 0U : 1U; pass < 2U && !sent; ++pass)
            for (std::size_t i = 0U; i < driver.dp_count && !sent; ++i) {
                auto& dp = driver.dps[i];
                if (!dp.to_set || (pass == 0U && dp.id != mode_dp))
                    continue;
                if (dp.rxed_length != dp.desired_length ||
                    std::memcmp(dp.rxed.data(), dp.desired.data(), dp.desired_length) != 0) {
#if USE_PWM
                    if (dim_held(dp.id))
                        continue;
#endif
                    send_state(dp.id, dp.type, dp.desired.data(), dp.desired_length);
                    dp.to_set = false;
                    driver.expected_response = kCmdState;
                    wait_for(300, Startup::send_cmd, Startup::wait_ack_cmd);
                    sent = true;
                } else {
                    AddLog(LogLevel::debug, "TYA: DPset ign-same value dp%u=0x%x,%u",
                        unsigned(dp.id), unsigned(dp.desired[0]), unsigned(dp.desired_length));
                    dp.to_set = false;
                }
            }
        if (!sent && driver.send_heartbeat) {
            driver.send_heartbeat = false;
            driver.startup_state = Startup::send_heartbeat;
        }
        break;
    }
    case Startup::wait_ack_cmd:
        if (cmd == driver.expected_response)
            wait_for(0, Startup::send_cmd, Startup::send_cmd);
        break;
    case Startup::send_heartbeat:
        if (cmd >= 0)
            break;
        send_command(kCmdHeartbeat);
        wait_for(1000, Startup::init, Startup::wait_ack_heartbeat);
        break;
    case Startup::wait_ack_heartbeat:
        if (cmd == kCmdHeartbeat) {
            // A zero heartbeat means the MCU restarted.
            wait_for(0, Startup::send_cmd,
                length > 0U && payload[0] == 0U ? Startup::init : Startup::send_cmd);
        }
        break;
    }
    if (state != driver.startup_state)
        AddLog(LogLevel::debug_more, "TYA: %u->%u-%u/%u", unsigned(state),
            unsigned(driver.startup_state), unsigned(driver.sends), unsigned(driver.receives));
    driver.in_state_machine = false;
}

/** @brief Keeps the MCU's last value of a data point, as TuyaStoreRxedDP. */
void store_rxed(const std::uint8_t id, const std::uint8_t type, const std::uint8_t* const data,
    const std::size_t length) {
    StoredDp* dp = nullptr;
    for (std::size_t i = 0U; i < driver.dp_count; ++i)
        if (driver.dps[i].id == id)
            dp = &driver.dps[i];
    if (dp == nullptr) {
        if (driver.dp_count >= kMaxStoredDps) {
            AddLog(LogLevel::error, "TYA: Max stored DPs exceeded dpId=%u type %u", unsigned(id),
                unsigned(type));
            return;
        }
        dp = &driver.dps[driver.dp_count++];
        *dp = {};
        dp->id = id;
    } else if (dp->type != type) {
        AddLog(LogLevel::error, "TYA: Type change in rxed dpId=%u type %u -> %u", unsigned(id),
            unsigned(dp->type), unsigned(type));
    }
    dp->type = type;
    if (length <= kMaxStringSize) {
        std::memcpy(dp->rxed.data(), data, length);
        dp->rxed_length = static_cast<std::uint8_t>(length);
    } else {
        AddLog(LogLevel::error, "TYA: DP len exceeded dpId=%u type %u (%u > %u", unsigned(id),
            unsigned(type), unsigned(length), unsigned(kMaxStringSize));
    }
}

#if USE_PWM
/** @brief Reports whether a value for @p id still waits to be sent and differs from the
 * MCU's last report.
 */
[[nodiscard]] bool value_pending(const std::uint8_t id) {
    for (std::size_t i = 0U; i < driver.dp_count; ++i) {
        const auto& dp = driver.dps[i];
        if (dp.id == id && dp.to_set &&
            (dp.rxed_length != dp.desired_length ||
                std::memcmp(dp.rxed.data(), dp.desired.data(), dp.desired_length) != 0))
            return true;
    }
    return false;
}

[[nodiscard]] bool light_on(const std::size_t endpoint) {
    return (driver.light->intent().desired_power & (1U << endpoint)) != 0U;
}

/** @brief Percentage of one independent channel, as Tasmota's current_color in 0..100. */
[[nodiscard]] std::uint16_t channel_percent(const LightSettings& value, const std::size_t channel) {
    return static_cast<std::uint16_t>(change_scale(
        light_color::effective_channel(value.channels[channel], value.dimmer), 0U, 255U, 0U, 100U));
}

/** @brief Applies a reported dimmer, CT or white value, as Tasmota runs Dimmer, Channel, CT or
 * White: only while the light is on unless SetOption54, never 0 unless SetOption131, and only
 * when it differs from the Light by more than one.
 */
void process_light_value(const std::uint8_t id, const std::uint8_t fn, const std::uint32_t value) {
    const bool first = fn == kFnDimmer || fn == kFnReport1;
    const bool second = fn == kFnDimmer2 || fn == kFnReport2;
    if (driver.light == nullptr || (!first && !second && fn != kFnCt && fn != kFnWhite))
        return;
    auto& light = *driver.light;
    const auto& tuya = driver.settings->tuya_settings();
    const auto independent = driver.layout.independent;
    const std::size_t index = first ? 0U : 1U;
    // As Tasmota, the second level is a CT unless the light has two dimmers.
    const bool ct = fn == kFnCt || (second && !independent);
    const auto level = static_cast<std::uint16_t>(
        ct ? change_scale(value, 0U, tuya.dimmer_max, kCtMax, kCtMin)
           : change_scale(value, tuya.dimmer_min, tuya.dimmer_max, 0U, 100U));
    AddLog(LogLevel::debug, "TYA: fn%u value %lu level %u", unsigned(fn),
        static_cast<unsigned long>(value), unsigned(level));
    driver.levels[index] = level;
    // As for relays: a newer value waits to be sent, so the Light keeps it. A report function
    // (27, 28) has its own data point; the value waits on the one that controls it.
    const auto control = dp_of(first ? kFnDimmer : ct ? kFnCt : second ? kFnDimmer2 : fn);
    if (value_pending(id) || (control != 0U && value_pending(control))) {
        AddLog(LogLevel::debug, "TYA: fn%u report kept back, dp%u pending", unsigned(fn),
            unsigned(control != 0U ? control : id));
        return;
    }
    std::uint8_t apply_off = 0U;
    std::uint8_t allow_zero = 0U;
    (void)driver.settings->option(54U, apply_off);
    (void)driver.settings->option(131U, allow_zero);
    const std::size_t endpoint = independent && second ? 1U : 0U;
    if ((!light_on(endpoint) && apply_off == 0U) || (level == 0U && allow_zero == 0U))
        return;
    const auto& current = light.intent();
    const auto differs = [level](const unsigned now) {
        return (level > now ? level - now : now - level) > 1U;
    };
    LightApiMutation mutation{};
    LightApiSnapshot snapshot{};
    if (independent && (first || second)) {
        if (!differs(channel_percent(current, endpoint)))
            return;
        mutation.set_brightness = true;
        mutation.brightness = light_color::brightness_value(static_cast<std::uint8_t>(level));
        (void)light_api_set(light, static_cast<std::uint8_t>(endpoint), mutation, snapshot,
            PowerOperationSource::switch_input);
    } else if (first) {
        if (!differs(current.dimmer))
            return;
        auto next = current;
        next.dimmer = static_cast<std::uint8_t>(level);
        (void)light.update(next, false);
    } else if (ct && (driver.layout.channels == 2U || driver.layout.channels == 5U)) {
        if (!differs(current.color_temperature))
            return;
        mutation.set_color_temperature = true;
        mutation.color_temperature = level;
        (void)light_api_set(light, 0U, mutation, snapshot, PowerOperationSource::switch_input);
    } else if (fn == kFnWhite && driver.layout.channels == 4U) {
        if (!differs(current.channels[3] != 0U ? current.dimmer : 0U))
            return;
        auto next = current;
        light_color::select_white(4U, next, static_cast<std::uint8_t>(level));
        (void)light.update(next, false);
    }
}

[[nodiscard]] unsigned hex_value(const char* const text, const std::size_t digits) {
    char part[5]{};
    std::memcpy(part, text, digits);
    return static_cast<unsigned>(std::strtoul(part, nullptr, 16));
}

/** @brief Applies a reported colour in the TuyaRGB format, as Tasmota runs HsbColor or Color. */
void process_light_color(
    const std::uint8_t id, const std::uint8_t* const data, const std::size_t length) {
    if (driver.light == nullptr || driver.layout.channels < 3U || value_pending(id))
        return;
    const auto* const text = reinterpret_cast<const char*>(data);
    LightApiMutation mutation{};
    if (driver.settings->tuya_settings().rgb_type <= 1U && length == 12U) {
        const std::array<std::uint16_t, 3> hsb{static_cast<std::uint16_t>(hex_value(text, 4U)),
            static_cast<std::uint16_t>(hex_value(text + 4, 4U) / 10U),
            static_cast<std::uint16_t>(hex_value(text + 8, 4U) / 10U)};
        if (hsb == driver.hsb)
            return;
        driver.hsb = hsb;
        mutation.set_hue = mutation.set_saturation = mutation.set_brightness = true;
        mutation.hue = std::min<std::uint16_t>(hsb[0], 360U);
        mutation.saturation = static_cast<std::uint8_t>(change_scale(hsb[1], 0U, 100U, 0U, 255U));
        mutation.brightness = light_color::brightness_value(
            static_cast<std::uint8_t>(std::min<std::uint16_t>(hsb[2], 100U)));
    } else if (driver.settings->tuya_settings().rgb_type > 1U && length == 14U) {
        if (strncasecmp(text, driver.rgb.data(), 6U) == 0)
            return;
        std::memcpy(driver.rgb.data(), text, 6U);
        mutation.set_rgb = true;
        for (std::size_t i = 0U; i < 3U; ++i)
            mutation.rgb[i] = static_cast<std::uint8_t>(hex_value(text + 2U * i, 2U));
    } else {
        return;
    }
    LightApiSnapshot snapshot{};
    (void)light_api_set(*driver.light, 0U, mutation, snapshot, PowerOperationSource::switch_input);
}
#endif

void publish(const Publication kind, const std::string_view leaf, const std::string_view payload) {
    if (driver.publisher != nullptr)
        driver.publisher(driver.publisher_context, kind, leaf, payload);
    else
        AddLog(LogLevel::debug, "TYA: %.*s", static_cast<int>(payload.size()), payload.data());
}

/** @brief Keeps a reported enum and publishes it as the result of TuyaEnum<x>, as Tasmota runs
 * that command; above the TuyaEnumList maximum that command answers nothing, so neither does this.
 */
void process_enum(const std::uint8_t fn, const std::uint8_t value) {
    const auto index = static_cast<std::size_t>(fn - kFnEnum1);
    if (driver.enum_state[index] == value)
        return;
    driver.enum_state[index] = value;
    if (value > driver.settings->tuya_settings().enum_max[index])
        return;
    char leaf[12];
    char payload[24];
    std::snprintf(leaf, sizeof leaf, "TuyaEnum%u", unsigned(index + 1U));
    std::snprintf(payload, sizeof payload, "{\"%s\":%u}", leaf, unsigned(value));
    publish(Publication::result, leaf, payload);
}

#if USE_TUYA_MCU_SENSORS
/** @brief One TuyaSNS field: Tasmota's kTuyaSensors name and the reading's quantity. As Tasmota
 * shows the MCU's numbers unchanged, temperatures and humidities are SensorQuantity::other: no
 * unit conversion, no offsets and no global temperature.
 */
struct SensorField final {
    const char* name;
    SensorQuantity quantity;
    const char* unit; ///< Unit of a SensorQuantity::other reading
};

// Tasmota's kTuyaSensors has an empty name before Timer1, so its JSON calls fnId 81 "" and
// 82..84 Timer1..Timer3; its web page shows Timer1..Timer4, as here.
constexpr std::array<SensorField, kSensorCount> kSensorFields{{
    {"Temperature", SensorQuantity::other, nullptr},
    {"TempSet", SensorQuantity::other, nullptr},
    {"Humidity", SensorQuantity::other, "%"},
    {"HumSet", SensorQuantity::other, "%"},
    {"Illuminance", SensorQuantity::illuminance, nullptr},
    {"TVOC", SensorQuantity::other, "ppm"},
    {"eCO2", SensorQuantity::carbon_dioxide, nullptr},
    {"CarbonDioxide", SensorQuantity::carbon_dioxide, nullptr},
    {"Gas", SensorQuantity::other, "%LEL"},
    {"PM", SensorQuantity::particulate, nullptr},
    {"Timer1", SensorQuantity::count, nullptr},
    {"Timer2", SensorQuantity::count, nullptr},
    {"Timer3", SensorQuantity::count, nullptr},
    {"Timer4", SensorQuantity::count, nullptr},
}};

/** @brief Decimals of a sensor function, as Tasmota: TempRes, TuyaTempSetRes, HumRes, else 0. */
[[nodiscard]] unsigned sensor_resolution(const std::uint8_t fn) {
    const auto& output = driver.settings->sensor_settings().output;
    if (fn == kFnTemp)
        return output.temperature_resolution;
    if (fn == kFnTempSet)
        return driver.settings->tuya_settings().temp_set_res;
    if (fn == kFnHum || fn == kFnHumSet)
        return output.humidity_resolution;
    return 0U;
}

/** @brief The reading of one reported function. As Tasmota's TuyaAdjustedTemperature, the MCU's
 * integer has as many decimals as the function's resolution.
 */
[[nodiscard]] SensorReading sensor_reading(const std::uint8_t fn) {
    const auto index = static_cast<std::size_t>(fn - kFnTemp);
    const auto& field = kSensorFields[index];
    const auto decimals = sensor_resolution(fn);
    std::int64_t value = driver.sensors[index];
    for (unsigned i = decimals; i < 3U; ++i)
        value *= 10;
    SensorReading reading{"TuyaSNS", field.name, field.quantity, 0};
    reading.unit = field.unit;
    reading.decimals = static_cast<std::uint8_t>(decimals);
    if (fn == kFnTemp || fn == kFnTempSet) {
        // As Tasmota's web page: SetOption8 names the unit; the MCU's value stays unconverted.
        std::uint8_t fahrenheit = 0U;
        (void)driver.settings->option(8U, fahrenheit);
        reading.unit = fahrenheit != 0U ? "°F" : "°C";
    }
    reading.value =
        static_cast<std::int32_t>(std::clamp<std::int64_t>(value, INT32_MIN, INT32_MAX));
    return reading;
}

/** @brief Keeps a reported sensor value and publishes it at once on SENSOR, as Tasmota: not in
 * the first 8 seconds, and with SetOption136 only the set points and timers.
 */
void process_sensor(const std::uint8_t fn, const std::int32_t value) {
    const auto index = static_cast<std::size_t>(fn - kFnTemp);
    const auto bit = static_cast<std::uint16_t>(1U << index);
    if ((driver.sensors_reported & bit) != 0U && driver.sensors[index] == value)
        return;
    driver.sensors[index] = value;
    driver.sensors_reported |= bit;
    std::uint8_t quiet = 0U;
    (void)driver.settings->option(136U, quiet);
    const bool always = fn > kFnPm25 || fn == kFnTempSet || fn == kFnHumSet;
    if (uptime_seconds() < 8U || (quiet != 0U && !always))
        return;
    const auto reading = sensor_reading(fn);
    if (!sensor_service().publish_event("TuyaSNS", &reading, 1U, {}))
        AddLog(LogLevel::debug, "TYA: TuyaSNS %s not published", reading.field);
}
#endif

#if USE_TUYA_MCU_ENERGY
/** @brief Keeps a reported energy value, scaled as Tasmota's TuyaProcessRxedDP: voltage in
 * 0.1 V, current in mA, power in 0.1 W and the total in 0.01 kWh; the combined raw data point
 * (36) has 0.1 V, mA and whole watts in bytes 0-1, 3-4 and 6-7.
 */
void process_energy(const std::uint8_t fn, const std::uint8_t type, const std::uint8_t* const data,
    const std::size_t length) {
    auto& energy = driver.energy;
    const auto clamp = [](const std::uint64_t value) {
        return static_cast<std::int32_t>(std::min<std::uint64_t>(value, INT32_MAX));
    };
    if (fn == kFnPowerCombined) {
        if (type != kTypeRaw || length < 8U) {
            AddLog(LogLevel::debug, "TYA: fn%u invalid length %u", unsigned(fn), unsigned(length));
            return;
        }
        const auto voltage = static_cast<unsigned>(data[0] << 8U | data[1]);
        const auto current = static_cast<unsigned>(data[3] << 8U | data[4]);
        const auto power = static_cast<unsigned>(data[6] << 8U | data[7]);
        energy.voltage_mv = static_cast<std::int32_t>(voltage * 100U);
        energy.current_ma = static_cast<std::int32_t>(current);
        energy.power_mw = static_cast<std::int64_t>(power) * 1000;
        AddLog(LogLevel::debug, "TYA: fn%u Voltage=%u Current=%u Active_Power=%u", unsigned(fn),
            voltage, current, power);
        return;
    }
    if (type != kTypeValue || length != 4U)
        return;
    const std::uint64_t value = read32(data);
    if (fn == kFnPower)
        energy.power_mw = static_cast<std::int64_t>(value * 100U);
    else if (fn == kFnCurrent)
        energy.current_ma = clamp(value);
    else if (fn == kFnVoltage)
        energy.voltage_mv = clamp(value * 100U);
    else if (fn == kFnPowerTotal) {
        energy.total_wh = value * 10U;
        energy.total_fresh = true;
    }
}
#endif

/** @brief Applies one reported data point, as TuyaProcessRxedDP: relays, switches, light,
 * sensors, enums and energy.
 */
void process_rxed(const std::uint8_t id, const std::uint8_t type, const std::uint8_t* const data,
    const std::size_t length) {
    const auto fn = function_of(driver.settings->tuya_settings(), id);
    if (type == kTypeEnum && length == 1U && fn >= kFnEnum1 && fn <= kFnEnum4) {
        process_enum(fn, data[0]);
        return;
    }
#if USE_TUYA_MCU_SENSORS
    if (type == kTypeValue && length == 4U && fn >= kFnTemp && fn <= kFnTimer4) {
        process_sensor(fn, static_cast<std::int32_t>(read32(data)));
        return;
    }
#endif
#if USE_TUYA_MCU_ENERGY
    if ((fn >= kFnPower && fn <= kFnVoltage) || fn == kFnPowerCombined || fn == kFnPowerTotal) {
        process_energy(fn, type, data, length);
        return;
    }
#endif
#if USE_PWM
    if (fn >= kFnDimmer && fn <= kFnWhite)
        driver.dim_delay_ms[fn == kFnDimmer2 ? 1U : 0U] = driver.default_dim_delay_ms;
    if (type == kTypeValue && length == 4U) {
        process_light_value(id, fn, read32(data));
        return;
    }
    if (type == kTypeString && fn == kFnRgb) {
        process_light_color(id, data, length);
        return;
    }
    if (type == kTypeEnum && length == 1U && fn == kFnModeSet) {
        driver.mode_color = data[0] != 0U;
        return;
    }
#endif
    if (type != kTypeBool || length < 1U)
        return;
    const bool value = data[0] != 0U;
    std::uint8_t relay = 0U;
    bool on = value;
    if (fn >= kFnRelay1 && fn <= kFnRelay8) {
        relay = static_cast<std::uint8_t>(fn - kFnRelay1 + 1U);
    } else if (fn >= kFnRelay1Inv && fn <= kFnRelay8Inv) {
        relay = static_cast<std::uint8_t>(fn - kFnRelay1Inv + 1U);
        on = !value;
    } else {
#if USE_SWITCHES
        // As SwitchSetState: the switch handler acts on the new level by its SwitchMode.
        if (fn >= kFnSwitch1 && fn <= kFnSwitch4) {
            const auto instance = static_cast<std::uint8_t>(fn - kFnSwitch1 + 1U);
            AddLog(LogLevel::debug, "TYA: fn%u Switch%u --> M%u", unsigned(fn), unsigned(instance),
                unsigned(value));
            driver.power->set_external_switch(instance, value);
        }
#endif
        return;
    }
    // Power index of the relay; a light relay switches its Light endpoint.
    std::uint8_t index = relay;
#if USE_PWM
    for (std::size_t endpoint = 0U; endpoint < driver.light_relays.size(); ++endpoint) {
        if (driver.light_relays[endpoint] != relay)
            continue;
        // As Tasmota's TuyaStoreRxedDP: dim values wait after a relay report, and only while on.
        driver.dim_delay_ms[endpoint] = driver.default_dim_delay_ms;
        if (on)
            driver.dim_enable |= static_cast<std::uint8_t>(1U << endpoint);
        else
            driver.dim_enable &= static_cast<std::uint8_t>(~(1U << endpoint));
        index = driver.light == nullptr ? 0U
                                        : driver.light->endpoints().index(PowerOwner::light,
                                              static_cast<std::uint8_t>(endpoint + 1U));
    }
    if (index == 0U)
        return;
#endif
    bool current = false;
    (void)driver.power->state(index, current);
    AddLog(LogLevel::debug, "TYA: fn%u Relay%u-->M%s T%s", unsigned(fn), unsigned(relay),
        on ? "On" : "Off", current ? "On" : "Off");
    if (on == current)
        return;
    // A different value still waits to be sent, such as the startup state: the MCU gets it
    // next and reports it back, so Power keeps the requested state meanwhile.
    for (std::size_t i = 0U; i < driver.dp_count; ++i) {
        const auto& dp = driver.dps[i];
        if (dp.id == id && dp.to_set &&
            (dp.desired_length != 1U || (dp.desired[0] != 0U) != value)) {
            AddLog(LogLevel::debug, "TYA: Relay%u report kept back, dp%u pending", unsigned(relay),
                unsigned(id));
            return;
        }
    }
    // The MCU switched; Power follows without sending that relay back. Other relays the
    // operation changes, such as an interlock partner, still reach the MCU.
    driver.reported_relay = relay;
    driver.reported_on = on;
    (void)driver.power->set(
        index, on ? PowerAction::on : PowerAction::off, PowerOperationSource::switch_input);
    driver.reported_relay = 0U;
}

/** @brief Walks the data points of a state report, as TuyaProcessStatePacket. */
void process_state_packet() {
    std::size_t start = 6U;
    while (start + 4U < driver.byte_counter) {
        const auto id = driver.buffer[start];
        const auto type = driver.buffer[start + 1U];
        const auto length =
            static_cast<std::size_t>(driver.buffer[start + 2U] << 8U | driver.buffer[start + 3U]);
        if (start + 4U + length > driver.byte_counter)
            break;
        const auto* const data = driver.buffer.data() + start + 4U;
        store_rxed(id, type, data, length);
        driver.in_report = true;
        process_rxed(id, type, data, length);
        driver.in_report = false;
        start += length + 4U;
    }
}

void process_normal_power_packet() {
    const auto cmd = driver.buffer[3];
    switch (cmd) {
    case kCmdQueryProduct: {
        const auto length = static_cast<int>(driver.buffer[4] << 8U | driver.buffer[5]);
        AddLog(LogLevel::info, "TYA: MCU Product ID: %.*s", length,
            reinterpret_cast<const char*>(driver.buffer.data() + 6U));
        break;
    }
    case kCmdHeartbeat:
        if (driver.buffer[6] == 0U) {
            AddLog(LogLevel::debug, "TYA: Detected MCU restart");
            driver.wifi_state = 0xFEU;
        }
        break;
    case kCmdState:
        process_state_packet();
        break;
    case kCmdWifiReset:
    case kCmdWifiSelect:
        // Acknowledge at once, as Tasmota. Newmota has no WifiConfig, so nothing else happens.
        send_command(cmd);
        AddLog(LogLevel::info, "TYA: MCU asked for a Wi-Fi reset; not supported");
        break;
    case kCmdWifiState:
        AddLog(LogLevel::debug, "TYA: RX WiFi LED set ACK");
        driver.wifi_state = static_cast<std::uint8_t>(driver.link);
        break;
    case kCmdMcuConf:
        // Mode 2 (LED and key on the module) would add Led1 and Button1 pins in Tasmota.
        AddLog(LogLevel::debug, "TYA: RX MCU configuration Mode=%u", unsigned(driver.buffer[5]));
        break;
#if USE_TIME
    case kCmdSetTime:
        driver.send_time = 3U;
        break;
#endif
    default:
        AddLog(LogLevel::debug, "TYA: RX unknown command");
        break;
    }
}

void process_low_power_packet() {
    switch (driver.buffer[3]) {
    case kCmdQueryProduct: {
        const auto length = static_cast<int>(driver.buffer[4] << 8U | driver.buffer[5]);
        AddLog(LogLevel::info, "TYA: MCU Product ID: %.*s", length,
            reinterpret_cast<const char*>(driver.buffer.data() + 6U));
        break;
    }
    case kLowPowerCmdState:
        process_state_packet();
        driver.send_success_next_second = true;
        break;
    default:
        break;
    }
}

/** @brief Bounded text for TuyaReceived; an overflow leaves it empty. */
struct Json final {
    std::array<char, 1024> data{};
    std::size_t size{};
    bool overflow{};

    void add(const char* const format, ...) __attribute__((format(printf, 2, 3))) {
        if (overflow)
            return;
        va_list arguments;
        va_start(arguments, format);
        const int written =
            std::vsnprintf(data.data() + size, data.size() - size, format, arguments);
        va_end(arguments);
        if (written < 0 || static_cast<std::size_t>(written) >= data.size() - size)
            overflow = true;
        else
            size += static_cast<std::size_t>(written);
    }

    /** @brief Adds MCU text as a JSON string body; Tasmota inserts it unescaped. */
    void text(const std::uint8_t* const bytes, const std::size_t count) {
        for (std::size_t i = 0U; i < count; ++i) {
            const auto byte = bytes[i];
            if (byte == '"' || byte == '\\')
                add("\\%c", byte);
            else if (byte < 0x20U)
                add("\\u%04x", unsigned(byte));
            else
                add("%c", byte);
        }
    }

    void hex(const std::uint8_t* const bytes, const std::size_t count) {
        for (std::size_t i = 0U; i < count; ++i)
            add("%02X", unsigned(bytes[i]));
    }
};

/** @brief Publishes every data point of a state frame on stat DpType<t>Id<d> (TuyaSend9).
 * Tasmota publishes only the frame's last data point and cuts values to 16 bits; strings keep
 * its 14 characters, and raw data goes out as hex instead of a stale number.
 */
void publish_dp_topics(const std::uint8_t* const buffer, const std::size_t total) {
    std::size_t start = 6U;
    while (start + 4U < total - 1U) {
        const auto dp_id = buffer[start];
        const auto dp_type = buffer[start + 1U];
        const auto dp_length =
            static_cast<std::size_t>(buffer[start + 2U] << 8U | buffer[start + 3U]);
        if (start + 4U + dp_length > total - 1U)
            break;
        const auto* const dp_data = buffer + start + 4U;
        char leaf[16];
        std::snprintf(leaf, sizeof leaf, "DpType%uId%u", unsigned(dp_type), unsigned(dp_id));
        char value[16]{};
        if ((dp_type == kTypeBool || dp_type == kTypeEnum) && dp_length == 1U) {
            std::snprintf(value, sizeof value, "%u", unsigned(dp_data[0]));
        } else if (dp_type == kTypeValue && dp_length == 4U) {
            std::snprintf(value, sizeof value, "%lu", static_cast<unsigned long>(read32(dp_data)));
        } else if (dp_type == kTypeString) {
            std::snprintf(value, 15U, "%.*s", static_cast<int>(dp_length),
                reinterpret_cast<const char*>(dp_data));
        } else {
            for (std::size_t i = 0U, shown = 0U; i < dp_length && shown + 2U < 15U;
                ++i, shown += 2U)
                std::snprintf(value + shown, sizeof value - shown, "%02X", unsigned(dp_data[i]));
        }
        publish(Publication::dp_topic, leaf, value);
        start += dp_length + 4U;
    }
}

/** @brief Handles one complete frame with a good checksum, as TuyaProcessCommand. */
void process_command(const std::uint32_t now) {
    const auto* const buffer = driver.buffer.data();
    const auto length = static_cast<std::size_t>(buffer[4] << 8U | buffer[5]);
    const auto total = length + 7U;
    const auto cmd = buffer[3];
    ++driver.receives;
    state_machine(cmd, length, buffer + 6U);

    Json json;
    json.add("{\"TuyaReceived\":{\"Data\":\"");
    json.hex(buffer, total);
    json.add("\",\"Cmnd\":%u", unsigned(cmd));
    if (length > 0U) {
        json.add(",\"CmndData\":\"");
        json.hex(buffer + 6U, length);
        json.add("\"");
        if (cmd == kCmdState) {
            std::size_t start = 6U;
            while (start + 4U < total - 1U) {
                const auto dp_id = buffer[start];
                const auto dp_type = buffer[start + 1U];
                const auto dp_length =
                    static_cast<std::size_t>(buffer[start + 2U] << 8U | buffer[start + 3U]);
                if (start + 4U + dp_length > total - 1U)
                    break;
                const auto* const dp_data = buffer + start + 4U;
                json.add(",\"DpType%uId%u\":", unsigned(dp_type), unsigned(dp_id));
                if (dp_type == kTypeBool && dp_length == 1U) {
                    json.add("%u", unsigned(dp_data[0]));
                } else if (dp_type == kTypeValue && dp_length == 4U) {
                    json.add("%lu", static_cast<unsigned long>(read32(dp_data)));
                } else if (dp_type == kTypeString) {
                    json.add("\"");
                    json.text(dp_data, dp_length);
                    json.add("\"");
                } else if (dp_type == kTypeEnum && dp_length == 1U) {
                    json.add("%u", unsigned(dp_data[0]));
                } else {
                    json.add("\"0x");
                    json.hex(dp_data, dp_length);
                    json.add("\"");
                }
                json.add(",\"%u\":{\"DpId\":%u,\"DpIdType\":%u,\"DpIdData\":\"", unsigned(dp_id),
                    unsigned(dp_id), unsigned(dp_type));
                json.hex(dp_data, dp_length);
                json.add("\"");
                if (dp_type == kTypeString) {
                    json.add(",\"Type3Data\":\"");
                    json.text(dp_data, dp_length);
                    json.add("\"");
                }
                json.add("}");
                start += dp_length + 4U;
            }
        }
    }
    json.add("}}");

    std::uint8_t received_mqtt = 0U;
    std::uint8_t exclude = 0U;
    (void)driver.settings->option(66U, received_mqtt);
    (void)driver.settings->option(137U, exclude);
    const std::string_view payload{json.data.data(), json.overflow ? 0U : json.size};
    if (json.overflow) {
        AddLog(LogLevel::debug, "TYA: TuyaReceived too long, not published");
    } else if (received_mqtt != 0U) {
        bool excluded = false;
        for (const auto excluded_cmd : kExcludedFromMqtt)
            excluded = excluded || excluded_cmd == cmd;
        if (!(excluded && exclude != 0U))
            publish(Publication::received, "TuyaReceived", payload);
    } else {
        AddLog(LogLevel::debug, "TYA: %.*s", static_cast<int>(payload.size()), payload.data());
    }

    if (cmd == kCmdState && driver.settings->tuya_settings().stat_topic && !driver.suspend_topic)
        publish_dp_topics(buffer, total);
    (void)now;

    if (driver.low_power_mode)
        process_low_power_packet();
    else
        process_normal_power_packet();
}

void dump_buffer() {
    if (!debug_logging())
        return;
    char hex[2U * 32U + 1U]{};
    const auto shown = driver.byte_counter < 32U ? driver.byte_counter : 32U;
    for (std::size_t i = 0U; i < shown; ++i)
        std::snprintf(hex + 2U * i, 3U, "%02X", unsigned(driver.buffer[i]));
    AddLog(LogLevel::debug, "TYA: Raw Data %s", hex);
}

/** @brief Assembles frames byte by byte, as TuyaProcessByte. */
void process_byte(const std::uint8_t byte, const std::uint32_t now) {
    if (driver.cmd_status != 0U && now - driver.last_byte_time > kByteTimeoutMs) {
        AddLog(LogLevel::error, "TYA: Reset by char timeout %lu > %lums",
            static_cast<unsigned long>(now - driver.last_byte_time),
            static_cast<unsigned long>(kByteTimeoutMs));
        dump_buffer();
        driver.cmd_status = 0U;
        ++driver.errors;
    }
    driver.last_byte_time = now;
    switch (driver.cmd_status) {
    case 0U:
        if (byte == 0x55U) {
            driver.cmd_status = 1U;
            driver.byte_counter = 0U;
            driver.buffer[driver.byte_counter++] = byte;
        } else {
            AddLog(LogLevel::error, "TYA: E55 0x%02X", unsigned(byte));
            ++driver.errors;
        }
        break;
    case 1U:
        if (byte == 0xAAU) {
            driver.cmd_status = 2U;
            driver.buffer[driver.byte_counter++] = byte;
            driver.cmd_checksum = 0xFFU;
        } else {
            AddLog(LogLevel::error, "TYA: EAA 0x%02X", unsigned(byte));
            ++driver.errors;
            driver.cmd_status = 0U;
        }
        break;
    case 2U:
        if (driver.byte_counter == 5U) {
            driver.data_length = static_cast<std::uint16_t>(driver.buffer[4] << 8U | byte);
            driver.cmd_status = driver.data_length == 0U ? 4U : 3U;
        }
        driver.cmd_checksum = static_cast<std::uint8_t>(driver.cmd_checksum + byte);
        driver.buffer[driver.byte_counter++] = byte;
        break;
    case 3U:
        driver.buffer[driver.byte_counter++] = byte;
        driver.cmd_checksum = static_cast<std::uint8_t>(driver.cmd_checksum + byte);
        if (driver.byte_counter == 6U + driver.data_length)
            driver.cmd_status = 4U;
        if (driver.byte_counter > kBufferSize - 3U) {
            AddLog(LogLevel::error, "TYA: input overflow");
            dump_buffer();
            driver.cmd_status = 0U;
            driver.byte_counter = 0U;
        }
        break;
    default:
        driver.buffer[driver.byte_counter++] = byte;
        if (driver.cmd_checksum == byte) {
            process_command(now);
        } else {
            AddLog(LogLevel::error, "TYA: Bad CS 0x%02X != 0x%02X", unsigned(byte),
                unsigned(driver.cmd_checksum));
            dump_buffer();
            ++driver.errors;
        }
        driver.cmd_status = 0U;
        driver.byte_counter = 0U;
        break;
    }
}

/** @brief Turns a relay change into a data point, unless the MCU itself reported it. */
bool post_relay(const std::uint8_t instance, const bool on) {
    if (instance == driver.reported_relay && on == driver.reported_on)
        return true;
    bool inverted = false;
    auto dp = dp_of(static_cast<std::uint8_t>(kFnRelay1 + instance - 1U));
    if (dp == 0U) {
        dp = dp_of(static_cast<std::uint8_t>(kFnRelay1Inv + instance - 1U));
        inverted = dp != 0U;
    }
    if (dp == 0U)
        return false;
    if (!post_bool(dp, on != inverted))
        return false;
    AddLog(LogLevel::debug, "TYA: post rpower%u v%u dp%u", unsigned(instance),
        unsigned(on != inverted), unsigned(dp));
    return true;
}

bool write_relay(void*, const std::uint8_t instance, const bool on) {
    return driver.active && post_relay(instance, on);
}

#if USE_PWM
/** @brief Drops the value still waiting for the data point of @p fn, if any. */
void cancel_pending(const std::uint8_t fn) {
    const auto dp = dp_of(fn);
    for (std::size_t i = 0U; i < driver.dp_count && dp != 0U; ++i)
        if (driver.dps[i].id == dp)
            driver.dps[i].to_set = false;
}

/** @brief Sends a dimmer or CT level scaled by DimmerRange, as Tasmota's LightSerialDuty. */
void send_level(const std::uint8_t fn, const std::size_t index, const std::uint16_t level) {
    const auto dp = dp_of(fn);
    if (dp == 0U)
        return;
    if (level == driver.levels[index]) {
        // Back at the MCU's level: a value still waiting, such as during TuyaDimDelay, is stale.
        // While a report is applied, a waiting value stays: it is newer than the report.
        if (!driver.in_report)
            cancel_pending(fn);
        return;
    }
    const auto& tuya = driver.settings->tuya_settings();
    auto duty = fn == kFnCt ? change_scale(level, kCtMin, kCtMax, tuya.dimmer_max, 0U)
                            : change_scale(level, 0U, 100U, tuya.dimmer_min, tuya.dimmer_max);
    // Dimmers act oddly below their minimum, so no level goes lower. Tasmota also raises the
    // CT, which then cannot reach its warm end.
    if (fn != kFnCt)
        duty = std::max<std::uint32_t>(duty, tuya.dimmer_min);
    AddLog(LogLevel::debug, "TYA: post fn%u level %u v%lu dp%u", unsigned(fn), unsigned(level),
        static_cast<unsigned long>(duty), unsigned(dp));
    post_value(dp, duty);
}

/** @brief Sends the colour in the TuyaRGB format when it changed, as Tasmota's TuyaSetChannels:
 * 0 and 1 `HHHHSSSSBBBB` (hue 0..360, saturation and brightness 0..1000) in upper or lower case,
 * 2 and 3 `RRGGBBFFFF6464`.
 */
void send_color(const LightSettings& value) {
    const auto dp = dp_of(kFnRgb);
    if (dp == 0U)
        return;
    const auto type = driver.settings->tuya_settings().rgb_type;
    char text[15]{};
    if (type <= 1U) {
        std::uint16_t hue = 0U;
        std::uint8_t saturation = 0U;
        light_color::rgb_to_hs(value.channels, hue, saturation);
        const std::array<std::uint16_t, 3> hsb{hue,
            static_cast<std::uint16_t>(change_scale(saturation, 0U, 255U, 0U, 100U)), value.dimmer};
        // The Light keeps RGB, so a reported hue can come back one off.
        bool same = true;
        for (std::size_t i = 0U; i < hsb.size(); ++i)
            same = same && std::abs(int(hsb[i]) - int(driver.hsb[i])) <= 1;
        if (same)
            return;
        driver.hsb = hsb;
        std::snprintf(text, sizeof text, type == 0U ? "%04X%04X%04X" : "%04x%04x%04x",
            unsigned(hsb[0]), unsigned(hsb[1]) * 10U, unsigned(hsb[2]) * 10U);
    } else {
        std::array<char, 7> rgb{};
        std::snprintf(rgb.data(), rgb.size(), type == 2U ? "%02X%02X%02X" : "%02x%02x%02x",
            unsigned(light_color::effective_channel(value.channels[0], value.dimmer)),
            unsigned(light_color::effective_channel(value.channels[1], value.dimmer)),
            unsigned(light_color::effective_channel(value.channels[2], value.dimmer)));
        if (strncasecmp(rgb.data(), driver.rgb.data(), 6U) == 0)
            return;
        driver.rgb = rgb;
        std::snprintf(text, sizeof text, type == 2U ? "%sFFFF6464" : "%sffff6464", rgb.data());
    }
    AddLog(LogLevel::debug, "TYA: TX RGB hex %s to dpId %u", text, unsigned(dp));
    (void)post_state(
        dp, kTypeString, reinterpret_cast<const std::uint8_t*>(text), std::strlen(text));
}

/** @brief Switches the MCU between white (0) and colour (1) before such values (ModeSet). */
void send_mode(const bool color) {
    const auto dp = dp_of(kFnModeSet);
    if (dp == 0U || driver.mode_color == color)
        return;
    driver.mode_color = color;
    post_enum(dp, color ? 1U : 0U);
}
#endif

/** @brief Splits `a,b` or `a b` into at most two tokens, as Tasmota's strtok_r(", ").
 * @return the token count; tokens point into @p text.
 */
std::size_t tokens(char* const text, char* (&parts)[2]) {
    std::size_t count = 0U;
    char* save = nullptr;
    for (char* token = strtok_r(text, ", ", &save); token != nullptr && count < 2U;
        token = strtok_r(nullptr, ", ", &save))
        parts[count++] = token;
    return count;
}

CommandStatus tuya_mcu_command(
    const CommandRequest& request, CommandOutput& output, RestartController& restart) {
    if (!request.is_query()) {
        std::array<char, 32> text{};
        if (request.payload.size() >= text.size())
            return CommandStatus::invalid;
        std::memcpy(text.data(), request.payload.data(), request.payload.size());
        char* parts[2]{};
        std::array<std::uint8_t, 2> values{};
        const auto count = tokens(text.data(), parts);
        for (std::size_t i = 0U; i < count; ++i)
            values[i] = static_cast<std::uint8_t>(std::strtoul(parts[i], nullptr, 0));
        const auto fn = values[0];
        const auto dp = values[1];
        if (valid_function(fn)) {
            auto tuya = driver.settings->tuya_settings();
            // Adding a second dimmer removes CT, RGB and white, as in Tasmota.
            const bool adds_dual_dimmer =
                dp != 0U && ((fn == kFnDimmer2 && dp_of(tuya, kFnDimmer)) ||
                                (fn == kFnDimmer && dp_of(tuya, kFnDimmer2)));
            if (adds_dual_dimmer) {
                for (const auto removed : {kFnCt, kFnRgb, kFnWhite})
                    if (dp_of(tuya, removed) != 0U)
                        add_function(tuya, removed, 0U);
            }
            add_function(tuya, fn, dp);
            // Two dimmers are independent channels (SetOption68), whichever mapping changed.
            const bool dual_dimmer = dp_of(tuya, kFnDimmer) != 0U && dp_of(tuya, kFnDimmer2) != 0U;
            if (driver.settings->set_option(68U, dual_dimmer ? 1U : 0U) ==
                SettingsUpdateResult::failed)
                return CommandStatus::failed;
            if (driver.settings->set_tuya_settings(tuya) == SettingsUpdateResult::failed)
                return CommandStatus::failed;
            (void)restart.schedule(RestartTarget::current, platform::monotonic_milliseconds());
        } else {
            AddLog(LogLevel::error, "TYA: TuyaMcu Invalid function id=%u", unsigned(fn));
        }
    }
    if (!output.write("{\"TuyaMCU\":["))
        return CommandStatus::failed;
    bool added = false;
    for (const auto& entry : driver.settings->tuya_settings().functions) {
        if (entry.function == 0U)
            continue;
        if (!output.format("%s{\"fnId\":%u,\"dpId\":%u}", added ? "," : "",
                unsigned(entry.function), unsigned(entry.dp)))
            return CommandStatus::failed;
        added = true;
    }
    return output.write("]}") ? CommandStatus::handled : CommandStatus::failed;
}

CommandStatus tuya_send_command(const CommandRequest& request, CommandOutput& output) {
    const unsigned index = request.has_index ? request.index : 1U;
    if (index == 0U) {
        driver.suspend_topic = true;
        driver.topic_resume_at = platform::monotonic_milliseconds() + 1000U;
        send_command(kCmdQueryState);
    } else if (index <= 7U && !request.payload.empty()) {
        std::array<char, 160> text{};
        if (request.payload.size() >= text.size())
            return CommandStatus::invalid;
        std::memcpy(text.data(), request.payload.data(), request.payload.size());
        char* parts[2]{};
        const char* data = "";
        const auto count = tokens(text.data(), parts);
        const auto dp = count > 0U ? static_cast<std::uint8_t>(std::strtoul(parts[0], nullptr, 0))
                                   : std::uint8_t{0};
        if (count > 1U)
            data = parts[1];
        switch (index) {
        case 1U:
            (void)post_bool(dp, std::strtoul(data, nullptr, 0) != 0U);
            break;
        case 2U:
            post_value(dp, static_cast<std::uint32_t>(std::strtoull(data, nullptr, 0)));
            break;
        case 3U:
            (void)post_state(
                dp, kTypeString, reinterpret_cast<const std::uint8_t*>(data), std::strlen(data));
            break;
        case 4U:
            post_enum(dp, static_cast<std::uint32_t>(std::strtoul(data, nullptr, 0)));
            break;
        case 5U:
            post_hex(dp, kTypeString, data);
            break;
        case 6U: {
            const char* begin = std::strchr(data, 'x');
            if (begin == nullptr)
                begin = std::strchr(data, 'X');
            post_hex(dp, kTypeRaw, begin == nullptr ? data : begin + 1);
            break;
        }
        default: {
            // Any command with a hex payload, sent at once outside the state machine.
            std::array<std::uint8_t, 64> bytes{};
            const auto length = std::strlen(data) / 2U;
            if (length > bytes.size())
                return CommandStatus::invalid;
            send_command(dp, bytes.data(),
                static_cast<std::uint16_t>(hex_bytes(data, bytes.data(), length)));
            break;
        }
        }
    } else if (index == 8U) {
        driver.suspend_topic = true;
        driver.topic_resume_at = platform::monotonic_milliseconds() + 1000U;
        send_command(kCmdQueryProduct);
    } else if (index == 9U) {
        auto tuya = driver.settings->tuya_settings();
        tuya.stat_topic = !tuya.stat_topic;
        if (driver.settings->set_tuya_settings(tuya) == SettingsUpdateResult::failed)
            return CommandStatus::failed;
        AddLog(
            LogLevel::info, "TYA: TuyaMCU Stat Topic %s", tuya.stat_topic ? "enabled" : "disabled");
    }
    return output.format("{\"TuyaSend%u\":\"Done\"}", index) ? CommandStatus::handled
                                                             : CommandStatus::failed;
}

/** @brief TuyaEnum<1..4> [value]: sends an enum value up to its TuyaEnumList maximum when it
 * differs from the last one; without a value lists every mapped enum, as Tasmota.
 */
CommandStatus tuya_enum_command(const CommandRequest& request, CommandOutput& output) {
    const unsigned index = request.has_index ? request.index : 1U;
    const auto& tuya = driver.settings->tuya_settings();
    if (index < 1U || index > kEnumCount)
        return CommandStatus::invalid;
    if (request.is_query()) {
        // Lists every mapped enum, also when Enum1 is not mapped, where Tasmota answers nothing.
        bool added = false;
        for (std::size_t i = 0U; i < kEnumCount; ++i) {
            if (dp_of(tuya, static_cast<std::uint8_t>(kFnEnum1 + i)) == 0U)
                continue;
            if (!output.format("%s\"Enum%u\":%u", added ? "," : "{\"TuyaEnum\":{", unsigned(i + 1U),
                    unsigned(driver.enum_state[i])))
                return CommandStatus::failed;
            added = true;
        }
        if (!added)
            return CommandStatus::invalid;
        return output.write("}}") ? CommandStatus::handled : CommandStatus::failed;
    }
    const auto dp = dp_of(tuya, static_cast<std::uint8_t>(kFnEnum1 + index - 1U));
    if (dp == 0U)
        return CommandStatus::invalid;
    std::uint32_t value = 0U;
    if (!support::parse_unsigned_decimal(request.payload, tuya.enum_max[index - 1U], value))
        return CommandStatus::invalid;
    auto& state = driver.enum_state[index - 1U];
    if (value != state) {
        state = static_cast<std::uint8_t>(value);
        post_enum(dp, value);
    }
    return output.format("{\"TuyaEnum%u\":%u}", index, unsigned(state)) ? CommandStatus::handled
                                                                        : CommandStatus::failed;
}

/** @brief TuyaEnumList <1..4>,<1..31>: the highest value of an enum, saved. Lists the mapped
 * enums; other arguments only list, and without a mapped enum there is no answer, as Tasmota.
 */
CommandStatus tuya_enum_list_command(const CommandRequest& request, CommandOutput& output) {
    auto tuya = driver.settings->tuya_settings();
    if (!request.is_query()) {
        std::array<char, 32> text{};
        if (request.payload.size() >= text.size())
            return CommandStatus::invalid;
        std::memcpy(text.data(), request.payload.data(), request.payload.size());
        char* parts[2]{};
        std::array<unsigned long, 2> values{};
        const auto count = tokens(text.data(), parts);
        for (std::size_t i = 0U; i < count; ++i)
            values[i] = std::strtoul(parts[i], nullptr, 0);
        if (values[0] >= 1U && values[0] <= kEnumCount && values[1] >= 1U && values[1] <= 31U) {
            tuya.enum_max[values[0] - 1U] = static_cast<std::uint8_t>(values[1]);
            if (driver.settings->set_tuya_settings(tuya) == SettingsUpdateResult::failed)
                return CommandStatus::failed;
        }
    }
    bool added = false;
    for (std::size_t i = 0U; i < kEnumCount; ++i) {
        if (dp_of(tuya, static_cast<std::uint8_t>(kFnEnum1 + i)) == 0U)
            continue;
        if (!output.format("%s\"Enum%u\":%u", added ? "," : "{\"TuyaEnumList\":{", unsigned(i + 1U),
                unsigned(tuya.enum_max[i])))
            return CommandStatus::failed;
        added = true;
    }
    if (!added)
        return CommandStatus::invalid;
    return output.write("}}") ? CommandStatus::handled : CommandStatus::failed;
}

#if USE_TUYA_MCU_SENSORS
/** @brief TuyaTempSetRes <0..3>: decimals of the temperature set point (fnId 72), saved. */
CommandStatus tuya_temp_set_res_command(const CommandRequest& request, CommandOutput& output) {
    auto tuya = driver.settings->tuya_settings();
    if (!request.is_query()) {
        std::uint32_t resolution = 0U;
        if (!support::parse_unsigned_decimal(request.payload, 3U, resolution))
            return CommandStatus::invalid;
        tuya.temp_set_res = static_cast<std::uint8_t>(resolution);
        if (driver.settings->set_tuya_settings(tuya) == SettingsUpdateResult::failed)
            return CommandStatus::failed;
    }
    return output.format("{\"TuyaTempSetRes\":%u}", unsigned(tuya.temp_set_res))
               ? CommandStatus::handled
               : CommandStatus::failed;
}
#endif

#if USE_PWM
/** @brief TuyaRGB <0..3>: the colour format; only with a colour data point, as in Tasmota. */
CommandStatus tuya_rgb_command(const CommandRequest& request, CommandOutput& output) {
    auto tuya = driver.settings->tuya_settings();
    if (!request.is_query()) {
        std::uint32_t type = 0U;
        if (!support::parse_unsigned_decimal(request.payload, 3U, type) ||
            dp_of(tuya, kFnRgb) == 0U)
            return CommandStatus::invalid;
        tuya.rgb_type = static_cast<std::uint8_t>(type);
        if (driver.settings->set_tuya_settings(tuya) == SettingsUpdateResult::failed)
            return CommandStatus::failed;
    }
    return output.format("{\"TuyaRGB\":%u}", unsigned(tuya.rgb_type)) ? CommandStatus::handled
                                                                      : CommandStatus::failed;
}

/** @brief TuyaDimDelay <ms>: wait after a relay or dim report before sending dim values. As in
 * Tasmota it is not saved.
 */
CommandStatus tuya_dim_delay_command(const CommandRequest& request, CommandOutput& output) {
    if (request.has_index && request.index != 1U)
        return CommandStatus::invalid;
    if (request.is_query())
        return output.format("{\"TuyaDimDelay\":{\"dimdelay\":%d}}", driver.default_dim_delay_ms)
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    std::int32_t delay = 0;
    if (!support::parse_signed_decimal(request.payload, -600000, 600000, delay))
        return CommandStatus::invalid;
    driver.default_dim_delay_ms = delay;
    return output.write("{\"TuyaDimDelay\":\"Done\"}") ? CommandStatus::handled
                                                       : CommandStatus::failed;
}

/** @brief DimmerRange <min>,<max>: the MCU values of 0 and 100 %, saved; reversed values swap. */
CommandStatus dimmer_range_command(const CommandRequest& request, CommandOutput& output) {
    auto tuya = driver.settings->tuya_settings();
    if (!request.is_query()) {
        std::array<char, 32> text{};
        if (request.payload.size() >= text.size())
            return CommandStatus::invalid;
        std::memcpy(text.data(), request.payload.data(), request.payload.size());
        char* parts[2]{};
        std::array<unsigned long, 2> values{tuya.dimmer_min, tuya.dimmer_max};
        const auto count = tokens(text.data(), parts);
        for (std::size_t i = 0U; i < count; ++i)
            values[i] = std::min(std::strtoul(parts[i], nullptr, 0), 65535UL);
        tuya.dimmer_min = static_cast<std::uint16_t>(std::min(values[0], values[1]));
        tuya.dimmer_max = static_cast<std::uint16_t>(std::max(values[0], values[1]));
        if (driver.settings->set_tuya_settings(tuya) == SettingsUpdateResult::failed)
            return CommandStatus::failed;
    }
    return output.format("{\"DimmerRange\":{\"Min\":%u,\"Max\":%u}}", unsigned(tuya.dimmer_min),
               unsigned(tuya.dimmer_max))
               ? CommandStatus::handled
               : CommandStatus::failed;
}
#endif

} // namespace

bool initialize(const Hardware& hardware, Settings& settings, PowerService& power) {
    namespace gpio = generated::gpio;
    driver = {};
    PinId tx{};
    PinId rx{};
    for (std::size_t index = 0U; index < hardware.binding_count(); ++index) {
        const auto* binding = hardware.binding(index);
        if (!binding->active())
            continue;
        const auto base = binding->function >> gpio::kEncodingShift;
        if (base == gpio::kGpioTuyaTxBase)
            tx = binding->pin;
        else if (base == gpio::kGpioTuyaRxBase)
            rx = binding->pin;
    }
    if (!tx.valid() || !rx.valid())
        return false;
    driver.settings = &settings;
    driver.power = &power;

    auto tuya = settings.tuya_settings();
    bool relay_set = false;
    for (const auto& entry : tuya.functions)
        if (entry.dp != 0U &&
            ((entry.function >= kFnRelay1 && entry.function <= kFnRelay8) ||
                (entry.function >= kFnRelay1Inv && entry.function <= kFnRelay8Inv)))
            relay_set = true;
    if (!relay_set && dp_of(tuya, kFnDummy) == 0U) {
        // As Tasmota: the first relay exists by default; function 99 (dummy) removes it.
        add_function(tuya, kFnRelay1, 1U);
        if (settings.set_tuya_settings(tuya) == SettingsUpdateResult::failed)
            AddLog(LogLevel::error, "TYA: default Relay1 not saved");
    }
    for (const auto& entry : tuya.functions) {
        if (entry.dp == 0U)
            continue;
        if (entry.function >= kFnRelay1 && entry.function <= kFnRelay8)
            driver.relays |= 1U << (entry.function - kFnRelay1);
        else if (entry.function >= kFnRelay1Inv && entry.function <= kFnRelay8Inv)
            driver.relays |= 1U << (entry.function - kFnRelay1Inv);
        else if (entry.function >= kFnSwitch1 && entry.function <= kFnSwitch4)
            driver.switches |= 1U << (entry.function - kFnSwitch1);
    }
    driver.low_power_mode = dp_of(tuya, kFnLowPowerMode) != 0U;
#if USE_TUYA_MCU_ENERGY
    // As Tasmota's Xnrg32: the combined data point carries voltage and current too.
    const bool combined = dp_of(tuya, kFnPowerCombined) != 0U;
    driver.energy.voltage = combined || dp_of(tuya, kFnVoltage) != 0U;
    driver.energy.current = combined || dp_of(tuya, kFnCurrent) != 0U;
    driver.energy.total = dp_of(tuya, kFnPowerTotal) != 0U;
#endif
#if USE_PWM
    // Tasmota's light types: a dimmer, with CT, a second dimmer or RGB, and RGB with CT or white.
    if (dp_of(tuya, kFnDimmer) != 0U) {
        const bool rgb = dp_of(tuya, kFnRgb) != 0U;
        const bool ct = dp_of(tuya, kFnCt) != 0U;
        driver.layout.channels = rgb && ct                       ? 5U
                                 : rgb && dp_of(tuya, kFnWhite)  ? 4U
                                 : rgb                           ? 3U
                                 : ct || dp_of(tuya, kFnDimmer2) ? 2U
                                                                 : 1U;
        driver.layout.independent = !rgb && !ct && dp_of(tuya, kFnDimmer2) != 0U;
        driver.layout.mode = driver.layout.channels >= 4U && dp_of(tuya, kFnModeSet) != 0U;
    }
    // As Tasmota's Light.device, the light switches with the last relays; those relays become
    // its power and leave the relay endpoints.
    const std::size_t endpoints = driver.layout.channels == 0U ? 0U
                                  : driver.layout.independent  ? 2U
                                                               : 1U;
    std::array<std::uint8_t, 2> last{};
    std::size_t found = 0U;
    for (std::uint8_t relay = 8U; relay >= 1U && found < endpoints; --relay)
        if ((driver.relays & (1U << (relay - 1U))) != 0U)
            last[found++] = relay;
    for (std::size_t i = 0U; i < found; ++i) {
        driver.light_relays[i] = last[found - 1U - i];
        driver.relays &= ~(1U << (driver.light_relays[i] - 1U));
    }
    for (std::size_t i = found; i < driver.light_relays.size(); ++i)
        driver.dim_enable |= static_cast<std::uint8_t>(1U << i);
#endif

    std::uint8_t fast_baud = 0U;
    (void)settings.option(97U, fast_baud);
    serial::Config config{};
    config.rx = rx;
    config.tx = tx;
    config.baud = fast_baud != 0U ? 115200U : 9600U;
    if (serial::open_tuya(config, driver.handle) != serial::Result::success) {
        AddLog(LogLevel::error, "TYA: Serial port unavailable");
        driver = {};
        return false;
    }
    driver.active = true;
    power.attach_external_relays(driver.relays, write_relay, nullptr);
#if USE_SWITCHES
    power.attach_external_switches(driver.switches);
#endif
    const auto now = platform::monotonic_milliseconds();
    driver.suspend_topic = true;
    driver.topic_resume_at = now + 1000U;
    driver.next_tick = now + 100U;
    driver.next_second = now + 1000U;
    AddLog(LogLevel::debug, "TYA: Request MCU configuration at %lu bps",
        static_cast<unsigned long>(config.baud));
    return true;
}

bool active() {
    return driver.active;
}

#if USE_TUYA_MCU_ENERGY
bool energy_mapped() {
    return driver.active && (dp_of(kFnPower) != 0U || dp_of(kFnPowerCombined) != 0U);
}

EnergyReading read_energy() {
    const auto reading = driver.energy;
    driver.energy.total_fresh = false;
    return reading;
}
#endif

#if USE_PWM
LightLayout light_layout() {
    return driver.active ? driver.layout : LightLayout{};
}

void attach_light(LightService& light) {
    driver.light = &light;
}

void write_light() {
    if (!driver.active || driver.light == nullptr)
        return;
    const auto& value = driver.light->intent();
    for (std::size_t endpoint = 0U; endpoint < driver.light_relays.size(); ++endpoint)
        if (driver.light_relays[endpoint] != 0U)
            (void)post_relay(driver.light_relays[endpoint], light_on(endpoint));
    // As Tasmota, dim values go out only while on; switching off is the relay's job.
    if (driver.layout.independent) {
        if (light_on(0U))
            send_level(kFnDimmer, 0U, channel_percent(value, 0U));
        if (light_on(1U))
            send_level(kFnDimmer2, 1U, channel_percent(value, 1U));
        return;
    }
    if (!light_on(0U))
        return;
    const auto channels = driver.layout.channels;
    // Setting a colour clears white and the reverse, so the Light shows one mode at a time.
    const bool color = driver.light->shows_color();
    if (channels >= 4U) {
        // Values of the mode being left may still wait, as during TuyaDimDelay; sent after the
        // new mode, they would switch the MCU back.
        if (color) {
            for (const auto fn : {kFnDimmer, kFnCt, kFnWhite})
                cancel_pending(fn);
        } else {
            cancel_pending(kFnRgb);
        }
        send_mode(color);
    }
    if (color) {
        send_color(value);
        return;
    }
    // Forget the colour, so the MCU returning to it is applied again.
    driver.hsb.fill(0xFFFFU);
    driver.rgb[0] = '\0';
    if (channels == 4U) {
        send_level(kFnWhite, 1U, value.dimmer);
    } else {
        send_level(kFnDimmer, 0U, value.dimmer);
        if (channels != 1U)
            send_level(kFnCt, 1U, value.color_temperature);
    }
}
#endif

void set_publisher(const Publisher publisher, void* const context) {
    driver.publisher = publisher;
    driver.publisher_context = context;
}

#if USE_TIME
void set_time_source(TimeService* const time) {
    driver.time = time;
}
#endif

#if USE_TUYA_MCU_SENSORS
bool visit_sensor_readings(const SensorReadingVisitor visitor, void* const context) {
    if (!driver.active)
        return true;
    for (std::uint8_t fn = kFnTemp; fn <= kFnTimer4; ++fn) {
        if ((driver.sensors_reported & (1U << (fn - kFnTemp))) == 0U || dp_of(fn) == 0U)
            continue;
        if (!visitor(context, sensor_reading(fn)))
            return false;
    }
    return true;
}
#endif

void poll(const std::uint32_t now, const LinkState link) {
    if (!driver.active)
        return;
    driver.link = link;
    std::array<std::uint8_t, 32> bytes{};
    for (;;) {
        const auto received = serial::read(driver.handle, bytes.data(), bytes.size());
        if (received.result != serial::Result::success || received.count == 0U)
            break;
        for (std::size_t i = 0U; i < received.count; ++i)
            process_byte(bytes[i], now);
    }
    flush_tx();
    if (reached(now, driver.next_tick)) {
        driver.next_tick = now + 100U;
        if (driver.timeout_ms != 0) {
            driver.timeout_ms -= 100;
            if (driver.timeout_ms <= 0) {
                if (driver.startup_state != Startup::wait_ack_query)
                    AddLog(LogLevel::error, "TYA: Protocol timeout state %u -> %u",
                        unsigned(driver.startup_state), unsigned(driver.timeout_state));
                driver.startup_state = driver.timeout_state;
                driver.timeout_ms = 0;
            }
        }
#if USE_PWM
        for (std::size_t i = 0U; i < driver.dim_delay_ms.size(); ++i) {
            if (driver.dim_delay_ms[i] <= 0)
                continue;
            driver.dim_delay_ms[i] -= 100;
            if (driver.dim_delay_ms[i] <= 0) {
                driver.dim_delay_ms[i] = 0;
                AddLog(LogLevel::debug, "TYA: DimDelay%u->0", unsigned(i));
            }
        }
#endif
        state_machine();
    }
    if (reached(now, driver.next_second)) {
        driver.next_second = now + 1000U;
        // Tasmota also meant to send the time every minute, but a precedence slip in its
        // `!send_time & 2` test means it only answers the MCU's requests; so does Newmota.
        if (!driver.low_power_mode && ++driver.heartbeat_timer >= kHeartbeatIntervalSeconds) {
            driver.heartbeat_timer = 0U;
            driver.send_heartbeat = true;
        }
        if (reached(now, driver.topic_resume_at))
            driver.suspend_topic = false;
        if (driver.logged_errors != driver.errors) {
            AddLog(LogLevel::error, "TYA: Errorcnt %lu->%lu",
                static_cast<unsigned long>(driver.logged_errors),
                static_cast<unsigned long>(driver.errors));
            driver.logged_errors = driver.errors;
        }
    }
}

CommandStatus dispatch(
    const CommandRequest& request, CommandOutput& output, RestartController& restart) {
    if (!driver.active)
        return CommandStatus::unknown;
    if (support::ascii_iequals(request.name, "TuyaMCU"))
        return tuya_mcu_command(request, output, restart);
    if (support::ascii_iequals(request.name, "TuyaSend"))
        return tuya_send_command(request, output);
    if (support::ascii_iequals(request.name, "TuyaEnum"))
        return tuya_enum_command(request, output);
    if (support::ascii_iequals(request.name, "TuyaEnumList"))
        return tuya_enum_list_command(request, output);
#if USE_TUYA_MCU_SENSORS
    if (support::ascii_iequals(request.name, "TuyaTempSetRes"))
        return tuya_temp_set_res_command(request, output);
#endif
#if USE_PWM
    if (support::ascii_iequals(request.name, "TuyaRGB"))
        return tuya_rgb_command(request, output);
    if (support::ascii_iequals(request.name, "TuyaDimDelay"))
        return tuya_dim_delay_command(request, output);
    if (support::ascii_iequals(request.name, "DimmerRange"))
        return dimmer_range_command(request, output);
#endif
    return CommandStatus::unknown;
}

} // namespace newmota::tuya
