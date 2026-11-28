/*
  settings.cpp - persistent settings implementation for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Precision default reference: Tasmota settings.ino/support_command.ino,
  Copyright (C) Theo Arends, GPL version 3 or later.
  StateText presentation reference: Tasmota support_command.ino/support_tasmota.ino,
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
*/

/** @file settings.cpp
 * @brief Transactional tagged settings and feature-independent presentation retention.
 */
#include "services/settings/settings.h"

#if USE_SENSOR_SETTINGS
#include "drivers/sensors/sensor_service.h"
#endif
#include "core/storage/filesystem.h"
#include "core/storage/record_slots.h"
#include "gpio_ids.h"
#include "newmota/log.h"
#include "newmota/platform/critical_settings.h"
#include "pin_topology.h"
#include "support/crc32.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace newmota {
namespace {

constexpr char kSettingsDirectory[] = "/.newmota";
/**
 * The two settings slots. A save rewrites the slot that does not hold the loaded generation,
 * and load() selects the newest valid generation, so either slot can hold the newer record.
 */
constexpr RecordSlots kSlots{"/.newmota/settings.a", "/.newmota/settings.b"};
constexpr std::array<std::uint8_t, 4> kMagic{'N', 'M', 'S', 'T'};
constexpr std::uint16_t kFormatVersion = 1;
constexpr std::uint16_t kHeaderSize = 20;
constexpr std::uint16_t kSetOptionsTag = 1;
constexpr std::uint16_t kSetOptionsSize = 38;
constexpr std::uint16_t kWifiTag = 2;
constexpr std::uint16_t kWifiSize =
    Settings::kWifiSlotCount * (1 + Settings::kSsidCapacity + 1 + Settings::kPasswordCapacity);
constexpr std::uint16_t kWifiPreferenceTag = 10;
constexpr std::uint16_t kWifiPreferenceSize = 1;
constexpr std::uint16_t kTimersTag = 17;
constexpr std::uint16_t kEnergyOutputTag = 18;
constexpr std::uint16_t kEnergyOutputSize = 6;
constexpr std::uint16_t kStateTextTag = 20;
constexpr std::uint16_t kStateTextSize = 128;
constexpr std::uint16_t kGlobalTemperatureTag = 19;
constexpr std::uint16_t kGlobalTemperatureSize = 1;
constexpr std::uint16_t kSleepTag = 22;
constexpr std::uint16_t kSleepSize = 1;
constexpr std::uint16_t kWifiPowerTag = 23;
constexpr std::uint16_t kWifiPowerSize = 1;
constexpr std::uint16_t kButtonDebounceTag = 24;
constexpr std::uint16_t kButtonDebounceSize = 2;
constexpr std::uint16_t kButtonMqttTag = 25;
constexpr std::uint16_t kButtonMqttSize = 2 + MqttSettings::kTopicCapacity;
constexpr std::uint16_t kRgbwwTableTag = 26;
constexpr std::uint16_t kRgbwwTableSize = 5;
constexpr std::uint16_t kLedCurrentTag = 27;
constexpr std::uint16_t kLedCurrentSize = 2;
constexpr std::uint16_t kSwitchTag = 28;
constexpr std::uint16_t kSwitchSize = 2 + Settings::kSwitchCount;
constexpr std::uint16_t kSwitchMqttTag = 29;
constexpr std::uint16_t kSwitchMqttSize = 2 + MqttSettings::kTopicCapacity;
constexpr std::uint16_t kSwitchTextTag = 30;
constexpr std::uint16_t kSwitchTextSize =
    Settings::kSwitchTextCount * Settings::kSwitchTextCapacity;
constexpr std::uint16_t kTuyaTag = 31;
constexpr std::uint16_t kTuyaSize = Settings::kTuyaFunctionCount * 2 + 11;
constexpr std::uint16_t kTimersSize = TimerSettings::kRecordSize;
constexpr std::uint16_t kTimeTag = 3;
constexpr std::uint16_t kTimeSize = 18;
constexpr std::uint16_t kNtpServersTag = 21;
constexpr std::uint16_t kNtpServersSize =
    Settings::kNtpServerCount * (1 + Settings::kNtpServerCapacity);
constexpr std::uint16_t kHardwareTag = 4;
constexpr std::uint16_t kHardwareSize =
    1 + kHardwareArchitectureCapacity + 1 + 1 + kUserTemplateNameCapacity + 1 + 4 + 1 +
    (kMaximumTemplateSlots * 2) + 1 + (kMaximumGpioOverrides * 4);
constexpr std::uint16_t kPowerTag = 5;
constexpr std::uint16_t kLoggingTag = 6;
constexpr std::uint16_t kLoggingSize = 2;
constexpr std::uint16_t kWebPasswordTag = 7;
constexpr std::uint16_t kWebPasswordSize = 1 + Settings::kPasswordCapacity;
constexpr std::uint16_t kMqttTag = 8;
constexpr std::size_t kMqttSize =
    2 + (1 + MqttSettings::kHostCapacity) + (1 + MqttSettings::kClientIdCapacity) +
    (1 + MqttSettings::kUsernameCapacity) + (1 + MqttSettings::kPasswordCapacity) +
    (1 + MqttSettings::kTopicCapacity) + 2 + 2;
constexpr std::uint16_t kMqttExtensionTag = 15;
constexpr std::size_t kMqttExtensionSize =
    1 + 1 + MqttSettings::kFullTopicCapacity + 3 * (1 + MqttSettings::kPrefixCapacity) +
    MqttSettings::kGroupCount * (1 + MqttSettings::kTopicCapacity) + 4 + 2;
constexpr std::uint16_t kTelemetryTag = 9;
constexpr std::uint16_t kTelemetrySize = 2;
constexpr std::uint16_t kDeviceNameTag = 11;
constexpr std::uint16_t kDeviceNameSize = 1 + Settings::kDeviceNameCapacity;
constexpr std::uint16_t kHostnameTag = 12;
constexpr std::uint16_t kHostnameSize = 1 + Settings::kHostnameCapacity;
constexpr std::uint16_t kOtaUpdateTag = 13;
constexpr std::uint16_t kOtaUpdateSize = 1 + Settings::kOtaUrlCapacity + 1;
constexpr std::uint16_t kLightTag = 14;
// A build without WS2812 writes V1 until it has strip fields to keep; V2 adds them.
constexpr std::uint16_t kLightV1Size = 4 + 2 + 32 + 5 + 2 + 5 + 1 + 64 + 40 + 5 + 2;
constexpr std::uint16_t kLightV2ExtensionSize = 6 + 4 + 2 + 1 + 2 + (4 * 5);
constexpr std::uint16_t kLightV2Size = kLightV1Size + kLightV2ExtensionSize;
constexpr std::uint16_t kLightSize = kLightV2Size;
#if USE_SENSOR_SETTINGS
constexpr std::uint16_t kSensorsSize = SensorSettings::kMaximumDriverId / 8;
constexpr RecordSlots kSensorSlots{"/.newmota/sensors.a", "/.newmota/sensors.b"};
constexpr std::array<std::uint8_t, 4> kSensorMagic{'N', 'M', 'S', 'P'};
constexpr std::uint8_t kSensorFileVersion = 1U;
constexpr std::size_t kSensorPresentationSize = 5;
// Magic (4), version (1), generation (4), disabled bitmap, presentation, CRC-32 (4).
constexpr std::size_t kSensorVersionOffset = 4;
constexpr std::size_t kSensorGenerationOffset = kSensorVersionOffset + 1;
constexpr std::size_t kSensorDisabledOffset = kSensorGenerationOffset + 4;
constexpr std::size_t kSensorOutputOffset = kSensorDisabledOffset + kSensorsSize;
constexpr std::size_t kSensorCrcOffset = kSensorOutputOffset + kSensorPresentationSize;
constexpr std::size_t kSensorFileSize = kSensorCrcOffset + 4;
#endif
constexpr std::uint16_t kPowerSize =
    7 + (kMaximumPowerOutputs * 2) + 2 + 2 + 1 + (kMaximumInterlockGroups * 4) + 3 + 3;
// Maximum encoded size; V1 Light records omit the strip extension.
constexpr std::size_t kEncodedSize =
    kHeaderSize + 4 + kSetOptionsSize + 4 + kWifiSize + 4 + kWifiPreferenceSize + 4 + kTimeSize +
    4 + kHardwareSize + 4 + kPowerSize + 4 + kLoggingSize + 4 + kWebPasswordSize + 4 + kMqttSize +
    4 + kTelemetrySize + 4 + kDeviceNameSize + 4 + kHostnameSize + 4 + kOtaUpdateSize + 4 +
    kLightSize + 4 + kMqttExtensionSize + 4 + kTimersSize + 4 + kEnergyOutputSize + 4 +
    kGlobalTemperatureSize + 4 + kStateTextSize + 4 + kNtpServersSize + 4 + kSleepSize + 4 +
    kWifiPowerSize + 4 + kButtonDebounceSize + 4 + kButtonMqttSize + 4 + kRgbwwTableSize + 4 +
    kLedCurrentSize + 4 + kSwitchSize + 4 + kSwitchMqttSize + 4 + kSwitchTextSize + 4 + kTuyaSize;
constexpr std::size_t kEncodedSizeWithoutWifi =
    kEncodedSize - 4 - kWifiSize - 4 - kWifiPreferenceSize;
constexpr std::size_t kReadCapacity = 4096;
constexpr std::size_t kMaximumSettingsFileSize = 4096;
constexpr std::array<std::uint8_t, 4> kCriticalMagic{'N', 'M', 'C', 'R'};
constexpr std::uint16_t kCriticalFormatVersion = 1;
constexpr std::uint16_t kCriticalWifiTag = 1;
constexpr std::uint16_t kCriticalWifiPreferenceTag = 2;
constexpr std::uint16_t kCriticalWifiPreferenceSize = 1;
constexpr std::uint16_t kCriticalWebPasswordTag = 3;
constexpr std::uint16_t kCriticalWebPasswordSize = 1 + Settings::kPasswordCapacity;
constexpr std::size_t kCriticalEncodedSize =
    kHeaderSize + 4 + kWifiSize + 4 + kCriticalWifiPreferenceSize + 4 + kCriticalWebPasswordSize;

static_assert(generated::hardware::kTemplateSlotCount <= kMaximumTemplateSlots);
static_assert(kEncodedSize <= kReadCapacity);
static_assert(kEncodedSizeWithoutWifi < kEncodedSize);
static_assert(kReadCapacity <= kMaximumSettingsFileSize);
static_assert(kCriticalEncodedSize <= platform::kCriticalSettingsCapacity);

// Settings operations run on small vendor main-task stacks. Keep their fixed
// transaction and I/O scratch in BSS rather than nesting it with LittleFS's
// own working buffers. Settings work is serialized on the application task;
// calls using shared scratch must not overlap or re-enter one another.
Settings::Data transaction_candidate;
// load() reads the slots into this buffer and save() encodes its record in it; save() does
// not read the record back, so the two uses never overlap.
std::array<std::uint8_t, kReadCapacity> record_scratch;
std::array<std::uint8_t, kCriticalEncodedSize> critical_encoded_scratch;
std::array<std::uint8_t, platform::kCriticalSettingsCapacity> critical_read_scratch;

/** @brief Copies live settings into the single non-stack transaction buffer. */
[[nodiscard]] Settings::Data& begin_transaction(const Settings::Data& current) {
    transaction_candidate = current;
    return transaction_candidate;
}

/** @brief Writes target-specific settings defaults directly into existing storage. */
void initialize_default_data(Settings::Data& data) {
    data.flags.fill(0U);
    data.flags[0] = 1U; // SetOption0: save and restore relay state.
    /** @brief Reuse constructor defaults for fresh/reset/decode buffers, not a zeroed variant.
     * Stored SetOptions records subsequently replace these bytes without zero substitution.
     */
    data.parameters = Settings::kDefaultParameters;
    data.energy_output = {};
    // Records written before Sleep existed get Tasmota's default.
    data.sleep = Settings::kDefaultSleep;
    data.wifi_power = Settings::kWifiPowerChipDefault;
    data.button_debounce = Settings::kDefaultButtonDebounce;
    data.switch_debounce = Settings::kDefaultButtonDebounce;
    data.switch_modes.fill(0U);
    for (auto& text : data.switch_texts)
        text.fill('\0');
    data.tuya = {};
    for (auto& ssid : data.ssids) {
        ssid.fill('\0');
    }
    data.ssid_lengths.fill(0U);
    for (auto& password : data.passwords) {
        password.fill('\0');
    }
    data.password_lengths.fill(0U);
    data.preferred_wifi_slot = 0U;
    data.web_password.fill('\0');
    data.web_password_length = 0U;
    data.time_rules[Settings::kStandardTimeRule] =
        Settings::default_time_rule(Settings::kStandardTimeRule);
    data.time_rules[Settings::kDaylightTimeRule] =
        Settings::default_time_rule(Settings::kDaylightTimeRule);
    data.timezone_offset_minutes = 60;
    data.time_format = 0U;
    for (std::uint8_t slot = 0; slot < Settings::kNtpServerCount; ++slot) {
        const auto address = Settings::default_ntp_server(slot);
        data.ntp_servers[slot].fill('\0');
        std::copy(address.begin(), address.end(), data.ntp_servers[slot].begin());
        data.ntp_server_lengths[slot] = static_cast<std::uint8_t>(address.size());
    }
#if USE_TIMERS
    data.timers = {};
#else
    data.timer_record.fill(0U);
    data.timer_record_present = false;
#endif
    data.timezone_automatic = false;
    data.serial_log_level = Settings::kDefaultSerialLogLevel;
    data.web_log_level = Settings::kDefaultWebLogLevel;
    data.tele_period_seconds = Settings::kDefaultTelePeriodSeconds;
#if USE_SENSOR_SETTINGS
    data.sensors = {};
#endif
    data.device_name.fill('\0');
    data.device_name_length = static_cast<std::uint8_t>(Settings::kDefaultDeviceName.size());
    for (std::size_t index = 0U; index < data.device_name_length; ++index) {
        data.device_name[index] = Settings::kDefaultDeviceName[index];
    }
    data.hostname.fill('\0');
    data.hostname_length = static_cast<std::uint8_t>(Settings::kDefaultHostname.size());
    for (std::size_t index = 0U; index < data.hostname_length; ++index) {
        data.hostname[index] = Settings::kDefaultHostname[index];
    }
    data.ota_url.fill('\0');
    data.ota_url_length = 0U;
    data.ota_update_pending = false;
    data.mqtt.reset();
    data.power = {};
    data.light = {};
    data.flags[0] |= 1U << 15U; // Tasmota defaults PWM outputs to Light ownership.
    data.flags[3] |= 1U << 1U;  // SetOption115: BLE runs when the image includes it.

    auto& hardware = data.hardware;
    hardware.architecture_storage.fill('\0');
    hardware.user_template_name_storage.fill('\0');
    hardware.user_template.fill(0U);
    hardware.overrides.fill(GpioOverride{});
    hardware.user_template_flag = 0U;
    hardware.module = 0U;
    hardware.architecture_length = 0U;
    hardware.user_template_name_length = 0U;
    hardware.user_template_base = 0U;
    hardware.user_template_size = 0U;
    hardware.override_count = 0U;
    (void)data.hardware.set_architecture(generated::hardware::kArchitecture);
    data.hardware.user_template_size =
        static_cast<std::uint8_t>(generated::hardware::kTemplateSlotCount);
#ifdef NEWMOTA_DEFAULT_TEMPLATE_GPIO
    // A build's own template replaces Generic, so a reset returns to it as well.
    static constexpr std::uint16_t kBuildTemplate[] = {NEWMOTA_DEFAULT_TEMPLATE_GPIO};
    static_assert(sizeof(kBuildTemplate) / sizeof(kBuildTemplate[0]) ==
                  generated::hardware::kTemplateSlotCount);
    (void)data.hardware.set_user_template_name(NEWMOTA_DEFAULT_TEMPLATE_NAME);
    data.hardware.user_template_flag = NEWMOTA_DEFAULT_TEMPLATE_FLAG;
    data.hardware.user_template_base = NEWMOTA_DEFAULT_TEMPLATE_BASE;
    for (std::size_t slot = 0; slot < data.hardware.user_template_size; ++slot) {
        data.hardware.user_template[slot] = kBuildTemplate[slot];
    }
#else
    (void)data.hardware.set_user_template_name("Generic");
    for (std::size_t slot = 0; slot < data.hardware.user_template_size; ++slot) {
        data.hardware.user_template[slot] = generated::gpio::kTemplateUserValue;
    }
#endif
}

/**
 * @brief Encodes a 16-bit integer in little-endian order.
 */
void write_u16(std::uint8_t* const destination, const std::uint16_t value) {
    destination[0] = static_cast<std::uint8_t>(value);
    destination[1] = static_cast<std::uint8_t>(value >> 8U);
}

/**
 * @brief Encodes a 32-bit integer in little-endian order.
 */
void write_u32(std::uint8_t* const destination, const std::uint32_t value) {
    destination[0] = static_cast<std::uint8_t>(value);
    destination[1] = static_cast<std::uint8_t>(value >> 8U);
    destination[2] = static_cast<std::uint8_t>(value >> 16U);
    destination[3] = static_cast<std::uint8_t>(value >> 24U);
}

/**
 * @brief Decodes a little-endian 16-bit integer.
 */
[[nodiscard]] std::uint16_t read_u16(const std::uint8_t* const source) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(source[0]) | static_cast<std::uint16_t>(source[1]) << 8U);
}

/**
 * @brief Decodes a little-endian 32-bit integer.
 */
[[nodiscard]] std::uint32_t read_u32(const std::uint8_t* const source) {
    return static_cast<std::uint32_t>(source[0]) | static_cast<std::uint32_t>(source[1]) << 8U |
           static_cast<std::uint32_t>(source[2]) << 16U |
           static_cast<std::uint32_t>(source[3]) << 24U;
}

/**
 * @brief Computes the settings-file CRC while excluding its stored CRC field.
 */
[[nodiscard]] std::uint32_t file_crc(const std::uint8_t* const data, const std::size_t size) {
    std::uint32_t crc = support::kCrc32InitialState;
    crc = support::crc32_update(crc, data, 16);
    crc = support::crc32_update(crc, data + kHeaderSize, size - kHeaderSize);
    return support::crc32_finalize(crc);
}

#if USE_SENSOR_SETTINGS
/** @brief Validates one sensor policy record and decodes its generation and fields. */
[[nodiscard]] bool decode_sensor_settings(const std::uint8_t* const bytes, const std::size_t size,
    SensorSettings& settings, std::uint32_t& generation) {
    if (size != kSensorFileSize || !std::equal(kSensorMagic.begin(), kSensorMagic.end(), bytes) ||
        bytes[kSensorVersionOffset] != kSensorFileVersion ||
        read_u32(bytes + kSensorCrcOffset) !=
            support::crc32_finalize(
                support::crc32_update(support::kCrc32InitialState, bytes, kSensorCrcOffset)))
        return false;

    const auto* const output = bytes + kSensorOutputOffset;
    const auto signed_byte = [](const std::uint8_t value) {
        return static_cast<std::int16_t>(
            value <= 127U ? value : static_cast<std::int16_t>(value) - 256);
    };
    const auto temperature_offset_value = signed_byte(output[3]);
    const auto humidity_offset_value = signed_byte(output[4]);
    if (output[0] > 3U || output[1] > 3U || output[2] > 3U || temperature_offset_value < -126 ||
        temperature_offset_value > 126 || humidity_offset_value < -100 ||
        humidity_offset_value > 100)
        return false;

    generation = read_u32(bytes + kSensorGenerationOffset);
    std::copy_n(bytes + kSensorDisabledOffset, kSensorsSize, settings.disabled.begin());
    settings.output.temperature_resolution = output[0];
    settings.output.humidity_resolution = output[1];
    settings.output.pressure_resolution = output[2];
    settings.output.temperature_offset_tenths = static_cast<std::int8_t>(temperature_offset_value);
    settings.output.humidity_offset_tenths = static_cast<std::int8_t>(humidity_offset_value);
    return true;
}

/** @brief Decodes the sensor policy slots and keeps the newest valid one. */
class SensorSettingsReader final : public RecordSlotReader {
  public:
    explicit SensorSettingsReader(SensorSettings& newest) : newest_(newest) {}

    bool check(const std::uint8_t, const std::uint8_t* const bytes, const std::size_t size,
        std::uint32_t& generation) override {
        return decode_sensor_settings(bytes, size, parsed_, generation);
    }

    void keep(const std::uint8_t, const std::uint8_t* const, const std::size_t) override {
        newest_ = parsed_;
    }

  private:
    SensorSettings parsed_{};
    SensorSettings& newest_;
};

/** @brief Reads the app-owned, CRC-protected driver policy slots into @p settings. */
[[nodiscard]] RecordSlotsLoad read_sensor_settings(SensorSettings& settings) {
    std::array<std::uint8_t, kSensorFileSize> bytes{};
    SensorSettingsReader reader(settings);
    return kSensorSlots.load(bytes.data(), bytes.size(), reader);
}

/** @brief Writes the sensor policy into the slot not holding the newest stored record. */
[[nodiscard]] bool save_sensor_settings(const SensorSettings& settings) {
    if (settings.output.temperature_resolution > 3U || settings.output.humidity_resolution > 3U ||
        settings.output.pressure_resolution > 3U ||
        settings.output.temperature_offset_tenths < -126 ||
        settings.output.temperature_offset_tenths > 126 ||
        settings.output.humidity_offset_tenths < -100 ||
        settings.output.humidity_offset_tenths > 100)
        return false;
    if (filesystem::ensure_directory(kSettingsDirectory) != filesystem::MutationResult::succeeded)
        return false;
    SensorSettings current{};
    const auto stored = read_sensor_settings(current);
    // A slot that could not be read may hold the newest record; never write over it blindly.
    if (!stored.readable())
        return false;

    std::array<std::uint8_t, kSensorFileSize> bytes{};
    std::copy(kSensorMagic.begin(), kSensorMagic.end(), bytes.begin());
    bytes[kSensorVersionOffset] = kSensorFileVersion;
    write_u32(bytes.data() + kSensorGenerationOffset, next_generation(stored.generation));
    std::copy(settings.disabled.begin(), settings.disabled.end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(kSensorDisabledOffset));
    auto* const output = bytes.data() + kSensorOutputOffset;
    output[0] = settings.output.temperature_resolution;
    output[1] = settings.output.humidity_resolution;
    output[2] = settings.output.pressure_resolution;
    output[3] = static_cast<std::uint8_t>(settings.output.temperature_offset_tenths);
    output[4] = static_cast<std::uint8_t>(settings.output.humidity_offset_tenths);
    write_u32(bytes.data() + kSensorCrcOffset,
        support::crc32_finalize(
            support::crc32_update(support::kCrc32InitialState, bytes.data(), kSensorCrcOffset)));
    return kSensorSlots.write(stored.slot, bytes.data(), bytes.size()) ==
           filesystem::MutationResult::succeeded;
}
#endif

/**
 * @brief Compares every persisted field of two timezone transition rules.
 */
[[nodiscard]] bool equal_time_rule(const TimeRule& left, const TimeRule& right) {
    return left.hemisphere == right.hemisphere && left.week == right.week &&
           left.month == right.month && left.day_of_week == right.day_of_week &&
           left.hour == right.hour && left.offset_minutes == right.offset_minutes;
}

/** @brief Counts set relay bits without relying on a compiler runtime helper. */
[[nodiscard]] std::uint8_t bit_count(std::uint32_t value) {
    std::uint8_t count = 0U;
    while (value != 0U) {
        value &= value - 1U;
        ++count;
    }
    return count;
}

/** @brief Validates disjoint interlock groups containing at least two relays. */
[[nodiscard]] bool valid_interlock_groups(
    const std::array<std::uint32_t, kMaximumInterlockGroups>& groups) {
    std::uint32_t assigned = 0U;
    for (const auto group : groups) {
        if (group == 0U) {
            continue;
        }
        if (bit_count(group) < 2U || (assigned & group) != 0U) {
            return false;
        }
        assigned |= group;
    }
    return true;
}

[[nodiscard]] constexpr std::uint16_t light_encoded_size(const LightSettings& light) {
#if USE_WS2812
    (void)light;
    return kLightV2Size;
#else
    return light.strip_extension_present ? kLightV2Size : kLightV1Size;
#endif
}

/**
 * @brief Serializes a settings record into the versioned tagged format.
 */
[[nodiscard]] std::size_t encode(const Settings::Data& data, const std::uint32_t generation,
    const bool include_wifi, std::array<std::uint8_t, kReadCapacity>& output) {
    // Zero only the bytes a record can use; the rest of the shared buffer is never written.
    std::fill_n(output.begin(), kEncodedSize, std::uint8_t{0});
    for (std::size_t index = 0; index < kMagic.size(); ++index) {
        output[index] = kMagic[index];
    }
    write_u16(output.data() + 4, kFormatVersion);
    write_u16(output.data() + 6, kHeaderSize);
    write_u32(output.data() + 8, generation);
    write_u16(output.data() + 12, 0);
    write_u16(output.data() + 14, 0);

    std::size_t position = kHeaderSize;
    write_u16(output.data() + position, kSetOptionsTag);
    write_u16(output.data() + position + 2, kSetOptionsSize);
    position += 4;
    for (const auto flags : data.flags) {
        write_u32(output.data() + position, flags);
        position += 4;
    }
    for (const auto parameter : data.parameters) {
        output[position++] = parameter;
    }

    if (include_wifi) {
        write_u16(output.data() + position, kWifiTag);
        write_u16(output.data() + position + 2, kWifiSize);
        position += 4;
        for (std::size_t slot = 0; slot < Settings::kWifiSlotCount; ++slot) {
            output[position++] = data.ssid_lengths[slot];
            for (const auto character : data.ssids[slot]) {
                output[position++] = static_cast<std::uint8_t>(character);
            }
            output[position++] = data.password_lengths[slot];
            for (const auto character : data.passwords[slot]) {
                output[position++] = static_cast<std::uint8_t>(character);
            }
        }
        write_u16(output.data() + position, kWifiPreferenceTag);
        write_u16(output.data() + position + 2U, kWifiPreferenceSize);
        position += 4U;
        output[position++] = data.preferred_wifi_slot;
    }

#if USE_TIMERS
    write_u16(output.data() + position, kTimersTag);
    write_u16(output.data() + position + 2, kTimersSize);
    position += 4;
    output[position++] = 1;
    output[position++] = data.timers.enabled;
    output[position++] = data.timers.sunrise;
    write_u32(output.data() + position, static_cast<std::uint32_t>(data.timers.latitude));
    position += 4;
    write_u32(output.data() + position, static_cast<std::uint32_t>(data.timers.longitude));
    position += 4;
    for (const auto& timer : data.timers.entries) {
        write_u16(output.data() + position, timer.time);
        position += 2;
        output[position++] = timer.window;
        output[position++] = timer.days;
        output[position++] = timer.output;
        output[position++] = timer.action;
        output[position++] = timer.mode;
        output[position++] = timer.enabled;
        output[position++] = timer.repeat;
    }
#else
    if (data.timer_record_present) {
        write_u16(output.data() + position, kTimersTag);
        write_u16(output.data() + position + 2, kTimersSize);
        position += 4;
        std::copy(data.timer_record.begin(), data.timer_record.end(), output.begin() + position);
        position += kTimersSize;
    }
#endif
    write_u16(output.data() + position, kTimeTag);
    write_u16(output.data() + position + 2, kTimeSize);
    position += 4;
    output[position++] = data.time_format;
    output[position++] = data.timezone_automatic ? 1 : 0;
    write_u16(output.data() + position, static_cast<std::uint16_t>(data.timezone_offset_minutes));
    position += 2;
    for (const auto& rule : data.time_rules) {
        output[position++] = rule.hemisphere;
        output[position++] = rule.week;
        output[position++] = rule.month;
        output[position++] = rule.day_of_week;
        output[position++] = rule.hour;
        write_u16(output.data() + position, static_cast<std::uint16_t>(rule.offset_minutes));
        position += 2;
    }

    write_u16(output.data() + position, kNtpServersTag);
    write_u16(output.data() + position + 2, kNtpServersSize);
    position += 4;
    for (std::uint8_t slot = 0; slot < Settings::kNtpServerCount; ++slot) {
        output[position++] = data.ntp_server_lengths[slot];
        for (const auto character : data.ntp_servers[slot])
            output[position++] = static_cast<std::uint8_t>(character);
    }

    write_u16(output.data() + position, kLoggingTag);
    write_u16(output.data() + position + 2, kLoggingSize);
    position += 4;
    output[position++] = data.serial_log_level;
    output[position++] = data.web_log_level;

    write_u16(output.data() + position, kWebPasswordTag);
    write_u16(output.data() + position + 2, kWebPasswordSize);
    position += 4;
    output[position++] = data.web_password_length;
    for (const auto character : data.web_password) {
        output[position++] = static_cast<std::uint8_t>(character);
    }

    write_u16(output.data() + position, kMqttTag);
    write_u16(output.data() + position + 2, kMqttSize);
    position += 4;
    output[position++] = data.mqtt.enabled ? 1U : 0U;
    output[position++] = data.mqtt.secure ? 1U : 0U;
    output[position++] = data.mqtt.host_length;
    for (const auto character : data.mqtt.host_storage) {
        output[position++] = static_cast<std::uint8_t>(character);
    }
    output[position++] = data.mqtt.client_id_length;
    for (const auto character : data.mqtt.client_id_storage) {
        output[position++] = static_cast<std::uint8_t>(character);
    }
    output[position++] = data.mqtt.username_length;
    for (const auto character : data.mqtt.username_storage) {
        output[position++] = static_cast<std::uint8_t>(character);
    }
    output[position++] = data.mqtt.password_length;
    for (const auto character : data.mqtt.password_storage) {
        output[position++] = static_cast<std::uint8_t>(character);
    }
    output[position++] = data.mqtt.topic_length;
    for (const auto character : data.mqtt.topic_storage) {
        output[position++] = static_cast<std::uint8_t>(character);
    }
    write_u16(output.data() + position, data.mqtt.port);
    position += 2;
    write_u16(output.data() + position, data.mqtt.keep_alive_seconds);
    position += 2;

    write_u16(output.data() + position, kMqttExtensionTag);
    write_u16(output.data() + position + 2, kMqttExtensionSize);
    position += 4;
    output[position++] = 1U;
    output[position++] = data.mqtt.full_topic_length;
    for (const auto c : data.mqtt.full_topic_storage)
        output[position++] = static_cast<std::uint8_t>(c);
    for (std::size_t i = 0; i < 3U; ++i) {
        output[position++] = data.mqtt.prefix_lengths[i];
        for (const auto c : data.mqtt.prefix_storage[i])
            output[position++] = static_cast<std::uint8_t>(c);
    }
    for (std::size_t i = 0; i < MqttSettings::kGroupCount; ++i) {
        output[position++] = data.mqtt.group_lengths[i];
        for (const auto c : data.mqtt.group_storage[i])
            output[position++] = static_cast<std::uint8_t>(c);
    }
    write_u16(output.data() + position, data.mqtt.retry_seconds);
    position += 2;
    write_u16(output.data() + position, data.mqtt.timeout_seconds);
    position += 2;
    const std::uint16_t mqtt_flags =
        (data.mqtt.response_topic ? 1U : 0U) | (data.mqtt.offline_lwt ? 2U : 0U) |
        (data.mqtt.group_topic_mode ? 4U : 0U) | (data.mqtt.only_json ? 8U : 0U) |
        (data.mqtt.no_retain ? 16U : 0U) | (data.mqtt.power_retain ? 32U : 0U) |
        (data.mqtt.info_retain ? 64U : 0U) | (data.mqtt.state_retain ? 128U : 0U) |
        (data.mqtt.status_retain ? 256U : 0U) | (data.mqtt.power_index ? 512U : 0U);
    write_u16(output.data() + position, mqtt_flags);
    position += 2;

    write_u16(output.data() + position, kTelemetryTag);
    write_u16(output.data() + position + 2, kTelemetrySize);
    position += 4;
    write_u16(output.data() + position, data.tele_period_seconds);
    position += 2;

    write_u16(output.data() + position, kDeviceNameTag);
    write_u16(output.data() + position + 2, kDeviceNameSize);
    position += 4;
    output[position++] = data.device_name_length;
    for (const auto character : data.device_name) {
        output[position++] = static_cast<std::uint8_t>(character);
    }

    write_u16(output.data() + position, kHostnameTag);
    write_u16(output.data() + position + 2, kHostnameSize);
    position += 4;
    output[position++] = data.hostname_length;
    for (const auto character : data.hostname) {
        output[position++] = static_cast<std::uint8_t>(character);
    }

    write_u16(output.data() + position, kOtaUpdateTag);
    write_u16(output.data() + position + 2, kOtaUpdateSize);
    position += 4;
    output[position++] = data.ota_url_length;
    for (const auto character : data.ota_url) {
        output[position++] = static_cast<std::uint8_t>(character);
    }
    output[position++] = data.ota_update_pending ? 1U : 0U;

    write_u16(output.data() + position, kLightTag);
    const auto light_size = light_encoded_size(data.light);
    const bool light_v2 = light_size == kLightV2Size;
    write_u16(output.data() + position + 2, light_size);
    position += 4;
    write_u32(output.data() + position, data.light.frequency);
    position += 4;
    write_u16(output.data() + position, data.light.range);
    position += 2;
    for (const auto duty : data.light.raw) {
        write_u16(output.data() + position, duty);
        position += 2;
    }
    for (const auto value : data.light.channels)
        output[position++] = value;
    write_u16(output.data() + position, data.light.color_temperature);
    position += 2;
    output[position++] = data.light.dimmer;
    output[position++] = data.light.speed;
    output[position++] = data.light.desired_power;
    output[position++] = data.light.gamma ? 1U : 0U;
    output[position++] = data.light.fade ? 1U : 0U;
    output[position++] = light_v2 ? 2U : 1U; // Light extension encoding version.
    for (const auto hz : data.light.channel_frequency) {
        write_u32(output.data() + position, hz);
        position += 4;
    }
    for (const auto& entry : data.light.palette)
        for (const auto channel : entry)
            output[position++] = channel;
    output[position++] = data.light.palette_count;
    output[position++] = data.light.palette_index;
    output[position++] = data.light.fixed_color_index;
    output[position++] = data.light.scheme;
    output[position++] = data.light.white_dimmer;
    write_u16(output.data() + position, data.light.wakeup_duration);
    position += 2;
    if (light_v2) {
#if USE_WS2812
        write_u16(output.data() + position, data.light.strip_pixels);
        position += 2;
        write_u16(output.data() + position, data.light.strip_rotation);
        position += 2;
        write_u16(output.data() + position, data.light.strip_step_pixels);
        position += 2;
        for (const auto width : data.light.strip_width)
            output[position++] = width;
        write_u16(output.data() + position, data.light.strip_height);
        position += 2;
        output[position++] = data.light.strip_type;
        output[position++] = data.light.strip_reverse ? 1U : 0U;
        output[position++] = data.light.strip_alternate ? 1U : 0U;
        for (const auto& color : data.light.strip_clock_colors)
            for (const auto channel : color)
                output[position++] = channel;
#else
        std::copy(data.light.strip_extension_raw.begin(), data.light.strip_extension_raw.end(),
            output.begin() + position);
        position += kLightV2ExtensionSize;
#endif
    }

    write_u16(output.data() + position, kHardwareTag);
    write_u16(output.data() + position + 2, kHardwareSize);
    position += 4;
    output[position++] = data.hardware.architecture_length;
    for (const auto character : data.hardware.architecture_storage) {
        output[position++] = static_cast<std::uint8_t>(character);
    }
    output[position++] = data.hardware.module;
    output[position++] = data.hardware.user_template_name_length;
    for (const auto character : data.hardware.user_template_name_storage) {
        output[position++] = static_cast<std::uint8_t>(character);
    }
    output[position++] = data.hardware.user_template_base;
    write_u32(output.data() + position, data.hardware.user_template_flag);
    position += 4;
    output[position++] = data.hardware.user_template_size;
    for (const auto assignment : data.hardware.user_template) {
        write_u16(output.data() + position, assignment);
        position += 2;
    }
    output[position++] = data.hardware.override_count;
    for (const auto& override : data.hardware.overrides) {
        write_u16(output.data() + position, override.pin.value());
        write_u16(output.data() + position + 2, override.function);
        position += 4;
    }

    write_u16(output.data() + position, kPowerTag);
    write_u16(output.data() + position + 2, kPowerSize);
    position += 4;
    write_u16(output.data() + position, data.power.save_data_seconds);
    position += 2;
    output[position++] = data.power.power_on_state;
    write_u32(output.data() + position, data.power.saved_state);
    position += 4;
    for (const auto pulse_time : data.power.pulse_times) {
        write_u16(output.data() + position, pulse_time);
        position += 2;
    }
    write_u16(output.data() + position, data.power.blink_time);
    position += 2;
    write_u16(output.data() + position, data.power.blink_count);
    position += 2;
    output[position++] = data.power.interlock_enabled ? 1U : 0U;
    for (const auto group : data.power.interlock_groups) {
        write_u32(output.data() + position, group);
        position += 4;
    }
    output[position++] = data.power.led_state;
    write_u16(output.data() + position, data.power.led_mask);
    position += 2;
    output[position++] = data.power.led_pwm_mask;
    output[position++] = data.power.led_pwm_on;
    output[position++] = data.power.led_pwm_off;
    write_u16(output.data() + position, kEnergyOutputTag);
    write_u16(output.data() + position + 2, kEnergyOutputSize);
    position += 4;
    for (const auto resolution : data.energy_output.decimals)
        output[position++] = resolution;
    write_u16(output.data() + position, kStateTextTag);
    write_u16(output.data() + position + 2, kStateTextSize);
    position += 4;
    for (const auto& alias : data.state_text)
        for (const auto character : alias)
            output[position++] = static_cast<std::uint8_t>(character);
    write_u16(output.data() + position, kGlobalTemperatureTag);
    write_u16(output.data() + position + 2, kGlobalTemperatureSize);
    position += 4;
    output[position++] = data.global_temperature_source;
    write_u16(output.data() + position, kSleepTag);
    write_u16(output.data() + position + 2, kSleepSize);
    position += 4;
    output[position++] = data.sleep;
    write_u16(output.data() + position, kWifiPowerTag);
    write_u16(output.data() + position + 2, kWifiPowerSize);
    position += 4;
    output[position++] = data.wifi_power;
    write_u16(output.data() + position, kButtonDebounceTag);
    write_u16(output.data() + position + 2, kButtonDebounceSize);
    position += 4;
    write_u16(output.data() + position, data.button_debounce);
    position += kButtonDebounceSize;
    write_u16(output.data() + position, kButtonMqttTag);
    write_u16(output.data() + position + 2, kButtonMqttSize);
    position += 4;
    output[position++] = data.mqtt.button_topic_length;
    output[position++] = data.mqtt.button_retain ? 1U : 0U;
    for (const auto character : data.mqtt.button_topic_storage)
        output[position++] = static_cast<std::uint8_t>(character);
    write_u16(output.data() + position, kRgbwwTableTag);
    write_u16(output.data() + position + 2, kRgbwwTableSize);
    position += 4;
    for (const auto value : data.light.rgbww_table)
        output[position++] = value;
    write_u16(output.data() + position, kLedCurrentTag);
    write_u16(output.data() + position + 2, kLedCurrentSize);
    position += 4;
    for (const auto value : data.light.led_current)
        output[position++] = value;
    write_u16(output.data() + position, kSwitchTag);
    write_u16(output.data() + position + 2, kSwitchSize);
    position += 4;
    write_u16(output.data() + position, data.switch_debounce);
    position += 2;
    for (const auto mode : data.switch_modes)
        output[position++] = mode;
    write_u16(output.data() + position, kSwitchMqttTag);
    write_u16(output.data() + position + 2, kSwitchMqttSize);
    position += 4;
    output[position++] = data.mqtt.switch_topic_length;
    output[position++] = data.mqtt.switch_retain ? 1U : 0U;
    for (const auto character : data.mqtt.switch_topic_storage)
        output[position++] = static_cast<std::uint8_t>(character);
    write_u16(output.data() + position, kSwitchTextTag);
    write_u16(output.data() + position + 2, kSwitchTextSize);
    position += 4;
    for (const auto& text : data.switch_texts)
        for (const auto character : text)
            output[position++] = static_cast<std::uint8_t>(character);
    write_u16(output.data() + position, kTuyaTag);
    write_u16(output.data() + position + 2, kTuyaSize);
    position += 4;
    for (const auto& entry : data.tuya.functions) {
        output[position++] = entry.function;
        output[position++] = entry.dp;
    }
    output[position++] = data.tuya.stat_topic ? 1U : 0U;
    write_u16(output.data() + position, data.tuya.dimmer_min);
    write_u16(output.data() + position + 2, data.tuya.dimmer_max);
    position += 4;
    output[position++] = data.tuya.rgb_type;
    output[position++] = data.tuya.temp_set_res;
    for (const auto maximum : data.tuya.enum_max)
        output[position++] = maximum;
    write_u16(output.data() + 12, static_cast<std::uint16_t>(position - kHeaderSize));
    write_u32(output.data() + 16, file_crc(output.data(), position));
    return position;
}

struct WifiFields final {
    std::array<std::array<char, Settings::kSsidCapacity>, Settings::kWifiSlotCount> ssids{};
    std::array<std::uint8_t, Settings::kWifiSlotCount> ssid_lengths{};
    std::array<std::array<char, Settings::kPasswordCapacity>, Settings::kWifiSlotCount> passwords{};
    std::array<std::uint8_t, Settings::kWifiSlotCount> password_lengths{};
    std::uint8_t preferred_wifi_slot{};
    std::array<char, Settings::kPasswordCapacity> web_password{};
    std::uint8_t web_password_length{};
    bool web_password_present{};
};

WifiFields critical_wifi_candidate;

/** @brief Installs validated Wi-Fi fields without copying unrelated settings. */
void install_wifi(const WifiFields& source, Settings::Data& destination) {
    destination.ssids = source.ssids;
    destination.ssid_lengths = source.ssid_lengths;
    destination.passwords = source.passwords;
    destination.password_lengths = source.password_lengths;
    destination.preferred_wifi_slot = source.preferred_wifi_slot;
    if (source.web_password_present) {
        destination.web_password = source.web_password;
        destination.web_password_length = source.web_password_length;
    }
}

/** @brief Encodes the OTA-critical Wi-Fi record owned by the settings service. */
void encode_critical_wifi(const Settings::Data& data, const std::uint32_t generation,
    std::array<std::uint8_t, kCriticalEncodedSize>& output) {
    output = {};
    for (std::size_t index = 0; index < kCriticalMagic.size(); ++index) {
        output[index] = kCriticalMagic[index];
    }
    write_u16(output.data() + 4, kCriticalFormatVersion);
    write_u16(output.data() + 6, kHeaderSize);
    write_u32(output.data() + 8, generation);
    write_u16(output.data() + 12, static_cast<std::uint16_t>(output.size() - kHeaderSize));
    write_u16(output.data() + 14, 0U);

    std::size_t position = kHeaderSize;
    write_u16(output.data() + position, kCriticalWifiTag);
    write_u16(output.data() + position + 2U, kWifiSize);
    position += 4U;
    for (std::size_t slot = 0; slot < Settings::kWifiSlotCount; ++slot) {
        output[position++] = data.ssid_lengths[slot];
        for (const auto character : data.ssids[slot]) {
            output[position++] = static_cast<std::uint8_t>(character);
        }
        output[position++] = data.password_lengths[slot];
        for (const auto character : data.passwords[slot]) {
            output[position++] = static_cast<std::uint8_t>(character);
        }
    }
    write_u16(output.data() + position, kCriticalWifiPreferenceTag);
    write_u16(output.data() + position + 2U, kCriticalWifiPreferenceSize);
    position += 4U;
    output[position++] = data.preferred_wifi_slot;
    write_u16(output.data() + position, kCriticalWebPasswordTag);
    write_u16(output.data() + position + 2U, kCriticalWebPasswordSize);
    position += 4U;
    output[position++] = data.web_password_length;
    for (const auto character : data.web_password) {
        output[position++] = static_cast<std::uint8_t>(character);
    }
    write_u32(output.data() + 16, file_crc(output.data(), output.size()));
}

/** @brief Validates and decodes one common OTA-critical settings record. */
[[nodiscard]] bool decode_critical_wifi(const std::uint8_t* const input, const std::size_t size,
    WifiFields& wifi, std::uint32_t& generation, bool& writable) {
    writable = false;
    wifi = {};
    if (size < kHeaderSize || input[0] != kCriticalMagic[0] || input[1] != kCriticalMagic[1] ||
        input[2] != kCriticalMagic[2] || input[3] != kCriticalMagic[3] ||
        read_u16(input + 4) != kCriticalFormatVersion || read_u16(input + 6) != kHeaderSize ||
        read_u16(input + 14) != 0U || read_u16(input + 12) != size - kHeaderSize ||
        read_u32(input + 16) != file_crc(input, size)) {
        return false;
    }

    bool found_wifi = false;
    bool found_wifi_preference = false;
    bool found_web_password = false;
    bool unknown_record = false;
    std::size_t position = kHeaderSize;
    while (position < size) {
        if (size - position < 4U) {
            return false;
        }
        const auto tag = read_u16(input + position);
        const auto length = read_u16(input + position + 2U);
        position += 4U;
        if (length > size - position) {
            return false;
        }
        if (tag == kCriticalWifiPreferenceTag) {
            if (found_wifi_preference || length != kCriticalWifiPreferenceSize) {
                return false;
            }
            const auto preferred_slot = input[position++];
            if (preferred_slot >= Settings::kWifiSlotCount) {
                return false;
            }
            wifi.preferred_wifi_slot = preferred_slot;
            found_wifi_preference = true;
            continue;
        }
        if (tag == kCriticalWebPasswordTag) {
            if (found_web_password || length != kCriticalWebPasswordSize) {
                return false;
            }
            const auto web_password_length = input[position++];
            if (web_password_length > Settings::kPasswordCapacity ||
                (web_password_length != 0U && web_password_length < 8U)) {
                return false;
            }
            wifi.web_password_length = web_password_length;
            for (auto& character : wifi.web_password) {
                character = static_cast<char>(input[position++]);
            }
            if (std::string_view{wifi.web_password.data(), web_password_length}.find('\0') !=
                std::string_view::npos) {
                return false;
            }
            wifi.web_password_present = true;
            found_web_password = true;
            continue;
        }
        if (tag != kCriticalWifiTag) {
            unknown_record = true;
            position += length;
            continue;
        }
        if (found_wifi || length != kWifiSize) {
            return false;
        }
        found_wifi = true;
        for (std::size_t slot = 0; slot < Settings::kWifiSlotCount; ++slot) {
            const auto ssid_length = input[position++];
            if (ssid_length > Settings::kSsidCapacity) {
                return false;
            }
            wifi.ssid_lengths[slot] = ssid_length;
            for (auto& character : wifi.ssids[slot]) {
                character = static_cast<char>(input[position++]);
            }
            const auto password_length = input[position++];
            if (password_length > Settings::kPasswordCapacity) {
                return false;
            }
            wifi.password_lengths[slot] = password_length;
            for (auto& character : wifi.passwords[slot]) {
                character = static_cast<char>(input[position++]);
            }
        }
    }
    if (!found_wifi) {
        return false;
    }
    generation = read_u32(input + 8);
    writable = !unknown_record;
    return true;
}

/**
 * @brief Checks one decoded timezone transition rule.
 */
[[nodiscard]] bool valid_time_rule(const TimeRule& rule) {
    return rule.hemisphere <= 1 && rule.week <= 4 && rule.month >= 1 && rule.month <= 12 &&
           rule.day_of_week >= 1 && rule.day_of_week <= 7 && rule.hour <= 23 &&
           rule.offset_minutes >= -900 && rule.offset_minutes <= 900;
}

/** @brief Checks bounds which make a decoded hardware record safe to inspect. */
[[nodiscard]] bool valid_hardware_shape(const HardwareConfiguration& configuration) {
    return configuration.architecture_length != 0U &&
           configuration.architecture_length <= configuration.architecture_storage.size() &&
           configuration.user_template_name_length <=
               configuration.user_template_name_storage.size() &&
           configuration.user_template_size <= configuration.user_template.size() &&
           configuration.override_count <= configuration.overrides.size();
}

/** @brief Reports whether a numeric assignment belongs to Tasmota's namespace. */
[[nodiscard]] constexpr bool valid_tasmota_gpio_value(const std::uint16_t function) {
    return function == generated::gpio::kTemplateUserValue ||
           (function >> generated::gpio::kEncodingShift) < generated::gpio::kSensorEnd;
}

/**
 * @brief Validates known bindings while retaining unimplemented Tasmota IDs.
 *
 * A stock Template may name a function whose Newmota driver is not compiled
 * yet. A value from the frozen Tasmota namespace remains inactive and
 * inspectable; known functions still have to pass pin, capability, and
 * duplicate checks.
 */
[[nodiscard]] bool valid_resolved_candidate(const HardwareConfiguration& configuration) {
    Hardware resolved{};
    if (!HardwareResolver::resolve(configuration, ResolutionPolicy::tolerant, resolved)
            .accepted()) {
        return false;
    }
    for (std::size_t index = 0; index < resolved.issue_count(); ++index) {
        if (resolved.issue(index)->code != ResolutionIssueCode::unknown_function) {
            return false;
        }
    }
    for (std::size_t index = 0; index < resolved.binding_count(); ++index) {
        const auto* const binding = resolved.binding(index);
        if (binding == nullptr || binding->active()) {
            continue;
        }
        if (binding->state != BindingState::unknown_function ||
            !valid_tasmota_gpio_value(binding->function)) {
            return false;
        }
    }
    return true;
}

/** @brief Validates both template views using the transaction's scratch storage. */
[[nodiscard]] bool valid_hardware_candidate(HardwareConfiguration& configuration) {
    if (!valid_hardware_shape(configuration) ||
        configuration.architecture() != generated::hardware::kArchitecture ||
        configuration.user_template_size != generated::hardware::kTemplateSlotCount ||
        configuration.architecture().find('\0') != std::string_view::npos ||
        configuration.user_template_name().find('\0') != std::string_view::npos) {
        return false;
    }
    // Only the transaction scratch is temporarily changed. Keeping another
    // full configuration here would stack with the command's candidate and
    // the resolver's fixed arrays on small vendor task stacks.
    const auto module = configuration.module;
    const auto override_count = configuration.override_count;
    configuration.module = 0U;
    configuration.override_count = 0U;
    const bool template_valid = valid_resolved_candidate(configuration);
    configuration.module = module;
    configuration.override_count = override_count;
    if (!template_valid) {
        return false;
    }

    return valid_resolved_candidate(configuration);
}

/** @brief Clears unused fixed-capacity fields before persisting a candidate. */
void canonicalize_hardware(HardwareConfiguration& configuration) {
    for (std::size_t index = configuration.architecture_length;
        index < configuration.architecture_storage.size(); ++index) {
        configuration.architecture_storage[index] = '\0';
    }
    for (std::size_t index = configuration.user_template_name_length;
        index < configuration.user_template_name_storage.size(); ++index) {
        configuration.user_template_name_storage[index] = '\0';
    }
    for (std::size_t index = configuration.user_template_size;
        index < configuration.user_template.size(); ++index) {
        configuration.user_template[index] = 0U;
    }
    for (std::size_t index = configuration.override_count; index < configuration.overrides.size();
        ++index) {
        configuration.overrides[index] = {};
    }
}

/** @brief Accepts disabled slots, DNS hostnames, and unambiguous dotted IPv4 addresses. */
[[nodiscard]] bool valid_ntp_server(std::string_view host) {
    if (host.size() > Settings::kNtpServerCapacity)
        return false;
    if (host.empty())
        return true;
    bool numeric = true;
    for (const char character : host) {
        if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') || character == '.' || character == '-'))
            return false;
        if ((character < '0' || character > '9') && character != '.')
            numeric = false;
    }
    unsigned labels = 0;
    while (!host.empty()) {
        const auto dot = host.find('.');
        const auto label = host.substr(0, dot);
        if (label.empty() || label.front() == '-' || label.back() == '-')
            return false;
        ++labels;
        if (numeric) {
            if (label.size() > 3 || (label.size() > 1 && label.front() == '0'))
                return false;
            unsigned value = 0;
            for (const char character : label)
                value = value * 10U + static_cast<unsigned>(character - '0');
            if (value > 255)
                return false;
        }
        if (dot == std::string_view::npos)
            break;
        host.remove_prefix(dot + 1);
        if (host.empty())
            return false;
    }
    return !numeric || labels == 4;
}

/**
 * @brief Validates and decodes one versioned settings record.
 * @return true when @p data and @p generation were populated.
 */
[[nodiscard]] bool decode(const std::uint8_t* const input, const std::size_t size,
    Settings::Data& data, std::uint32_t& generation, bool& writable, bool& wifi_present) {
    writable = false;
    wifi_present = false;
    if (size < kHeaderSize || input[0] != kMagic[0] || input[1] != kMagic[1] ||
        input[2] != kMagic[2] || input[3] != kMagic[3] || read_u16(input + 4) != kFormatVersion ||
        read_u16(input + 6) != kHeaderSize || read_u16(input + 14) != 0 ||
        read_u16(input + 12) != size - kHeaderSize ||
        read_u32(input + 16) != file_crc(input, size)) {
        return false;
    }

    initialize_default_data(data);
    auto& decoded = data;
    bool found_mqtt_extension = false;
    bool found_set_options = false;
    bool found_wifi = false;
    bool found_wifi_preference = false;
    bool found_time = false;
    bool found_ntp_servers = false;
    bool found_timers = false;
    bool found_logging = false;
    bool found_web_password = false;
    bool found_mqtt = false;
    bool found_telemetry = false;
    bool found_device_name = false;
    bool found_hostname = false;
    bool found_ota_update = false;
    bool found_light = false;
    bool found_energy_output = false;
    bool found_state_text = false;
    bool found_global_temperature = false;
    bool found_sleep = false;
    bool found_wifi_power = false;
    bool found_button_debounce = false;
    bool found_button_mqtt = false;
    bool found_rgbww_table = false;
    bool found_led_current = false;
    bool found_switch = false;
    bool found_switch_mqtt = false;
    bool found_switch_text = false;
    bool found_tuya = false;
    bool found_hardware = false;
    bool found_power = false;
    bool unknown_record = false;
    std::size_t position = kHeaderSize;
    while (position < size) {
        if (size - position < 4) {
            return false;
        }
        const auto tag = read_u16(input + position);
        const auto length = read_u16(input + position + 2);
        position += 4;
        if (length > size - position) {
            return false;
        }
        if (tag == kSetOptionsTag) {
            if (found_set_options || length != kSetOptionsSize) {
                return false;
            }
            found_set_options = true;
            for (auto& flags : decoded.flags) {
                flags = read_u32(input + position);
                position += 4;
            }
            for (auto& parameter : decoded.parameters) {
                parameter = input[position++];
            }
            continue;
        }
        if (tag == kWifiTag) {
            if (found_wifi || length != kWifiSize) {
                return false;
            }
            found_wifi = true;
            for (std::size_t slot = 0; slot < Settings::kWifiSlotCount; ++slot) {
                const auto ssid_length = input[position++];
                if (ssid_length > Settings::kSsidCapacity) {
                    return false;
                }
                decoded.ssid_lengths[slot] = ssid_length;
                for (auto& character : decoded.ssids[slot]) {
                    character = static_cast<char>(input[position++]);
                }
                const auto password_length = input[position++];
                if (password_length > Settings::kPasswordCapacity) {
                    return false;
                }
                decoded.password_lengths[slot] = password_length;
                for (auto& character : decoded.passwords[slot]) {
                    character = static_cast<char>(input[position++]);
                }
            }
            continue;
        }
        if (tag == kWifiPreferenceTag) {
            if (found_wifi_preference || length != kWifiPreferenceSize) {
                return false;
            }
            const auto preferred_slot = input[position++];
            if (preferred_slot >= Settings::kWifiSlotCount) {
                return false;
            }
            decoded.preferred_wifi_slot = preferred_slot;
            found_wifi_preference = true;
            continue;
        }
        if (tag == kTimersTag) {
            if (found_timers || length != kTimersSize)
                return false;
            found_timers = true;
#if USE_TIMERS
            if (input[position++] != 1U)
                return false;
            auto& timers = decoded.timers;
            const auto enabled = input[position++];
            timers.enabled = enabled != 0;
            timers.sunrise = input[position++];
            timers.latitude = static_cast<std::int32_t>(read_u32(input + position));
            position += 4;
            timers.longitude = static_cast<std::int32_t>(read_u32(input + position));
            position += 4;
            for (auto& timer : timers.entries) {
                timer.time = read_u16(input + position);
                position += 2;
                timer.window = input[position++];
                timer.days = input[position++];
                timer.output = input[position++];
                timer.action = input[position++];
                timer.mode = input[position++];
                const auto arm = input[position++];
                const auto repeat = input[position++];
                if (arm > 1 || repeat > 1)
                    return false;
                timer.enabled = arm != 0;
                timer.repeat = repeat != 0;
            }
            if (enabled > 1 || !timers.valid())
                return false;
#else
            decoded.timer_record_present = true;
            std::copy_n(input + position, kTimersSize, decoded.timer_record.begin());
            position += kTimersSize;
#endif
            continue;
        }
        if (tag == kNtpServersTag) {
            if (found_ntp_servers || length != kNtpServersSize)
                return false;
            found_ntp_servers = true;
            for (std::uint8_t slot = 0; slot < Settings::kNtpServerCount; ++slot) {
                const auto count = input[position++];
                if (count > Settings::kNtpServerCapacity)
                    return false;
                decoded.ntp_server_lengths[slot] = count;
                for (auto& character : decoded.ntp_servers[slot])
                    character = static_cast<char>(input[position++]);
                if (!valid_ntp_server({decoded.ntp_servers[slot].data(), count}))
                    return false;
            }
            continue;
        }
        if (tag == kTimeTag) {
            if (found_time || length != kTimeSize) {
                return false;
            }
            found_time = true;
            decoded.time_format = input[position++];
            const auto automatic = input[position++];
            if (automatic > 1) {
                return false;
            }
            decoded.timezone_automatic = automatic != 0;
            decoded.timezone_offset_minutes = static_cast<std::int16_t>(read_u16(input + position));
            position += 2;
            for (auto& rule : decoded.time_rules) {
                rule.hemisphere = input[position++];
                rule.week = input[position++];
                rule.month = input[position++];
                rule.day_of_week = input[position++];
                rule.hour = input[position++];
                rule.offset_minutes = static_cast<std::int16_t>(read_u16(input + position));
                position += 2;
            }
            if (decoded.time_format > 3 || decoded.timezone_offset_minutes < -839 ||
                decoded.timezone_offset_minutes > 899 || !valid_time_rule(decoded.time_rules[0]) ||
                !valid_time_rule(decoded.time_rules[1])) {
                return false;
            }
            continue;
        }
        if (tag == kWebPasswordTag) {
            if (found_web_password || length != kWebPasswordSize)
                return false;
            found_web_password = true;
            const auto count = input[position++];
            if (count > Settings::kPasswordCapacity || (count != 0 && count < 8))
                return false;
            decoded.web_password_length = count;
            for (auto& character : decoded.web_password) {
                character = static_cast<char>(input[position++]);
            }
            if (std::string_view{decoded.web_password.data(), count}.find('\0') !=
                std::string_view::npos)
                return false;
            continue;
        }
        if (tag == kLoggingTag) {
            if (found_logging || length != kLoggingSize) {
                return false;
            }
            found_logging = true;
            decoded.serial_log_level = input[position++];
            decoded.web_log_level = input[position++];
            if (decoded.serial_log_level > Settings::kMaximumLogLevel ||
                decoded.web_log_level > Settings::kMaximumLogLevel) {
                return false;
            }
            continue;
        }
        if (tag == kMqttTag) {
            if (found_mqtt || length != kMqttSize) {
                return false;
            }
            found_mqtt = true;
            auto& mqtt = decoded.mqtt;
            const auto enabled = input[position++];
            const auto secure = input[position++];
            if (enabled > 1U || secure > 1U)
                return false;
            mqtt.enabled = enabled != 0U;
            mqtt.secure = secure != 0U;
            mqtt.host_length = input[position++];
            for (auto& character : mqtt.host_storage) {
                character = static_cast<char>(input[position++]);
            }
            mqtt.client_id_length = input[position++];
            for (auto& character : mqtt.client_id_storage) {
                character = static_cast<char>(input[position++]);
            }
            mqtt.username_length = input[position++];
            for (auto& character : mqtt.username_storage) {
                character = static_cast<char>(input[position++]);
            }
            mqtt.password_length = input[position++];
            for (auto& character : mqtt.password_storage) {
                character = static_cast<char>(input[position++]);
            }
            mqtt.topic_length = input[position++];
            for (auto& character : mqtt.topic_storage) {
                character = static_cast<char>(input[position++]);
            }
            mqtt.port = read_u16(input + position);
            position += 2;
            mqtt.keep_alive_seconds = read_u16(input + position);
            position += 2;
            if (!mqtt.valid())
                return false;
            continue;
        }
        if (tag == kMqttExtensionTag) {
            if (found_mqtt_extension || length != kMqttExtensionSize || input[position++] != 1U)
                return false;
            found_mqtt_extension = true;
            auto& mqtt = decoded.mqtt;
            mqtt.full_topic_length = input[position++];
            for (auto& c : mqtt.full_topic_storage)
                c = static_cast<char>(input[position++]);
            for (std::size_t i = 0; i < 3U; ++i) {
                mqtt.prefix_lengths[i] = input[position++];
                for (auto& c : mqtt.prefix_storage[i])
                    c = static_cast<char>(input[position++]);
            }
            for (std::size_t i = 0; i < MqttSettings::kGroupCount; ++i) {
                mqtt.group_lengths[i] = input[position++];
                for (auto& c : mqtt.group_storage[i])
                    c = static_cast<char>(input[position++]);
            }
            mqtt.retry_seconds = read_u16(input + position);
            position += 2;
            mqtt.timeout_seconds = read_u16(input + position);
            position += 2;
            const auto flags = read_u16(input + position);
            position += 2;
            if (flags > 1023U)
                return false;
            mqtt.response_topic = flags & 1U;
            mqtt.offline_lwt = flags & 2U;
            mqtt.group_topic_mode = flags & 4U;
            mqtt.only_json = flags & 8U;
            mqtt.no_retain = flags & 16U;
            mqtt.power_retain = flags & 32U;
            mqtt.info_retain = flags & 64U;
            mqtt.state_retain = flags & 128U;
            mqtt.status_retain = flags & 256U;
            mqtt.power_index = flags & 512U;
            if (!mqtt.valid())
                return false;
            continue;
        }
        if (tag == kTelemetryTag) {
            if (found_telemetry || length != kTelemetrySize) {
                return false;
            }
            found_telemetry = true;
            decoded.tele_period_seconds = read_u16(input + position);
            position += 2;
            if (!TelemetrySettings::valid_period(decoded.tele_period_seconds)) {
                return false;
            }
            continue;
        }
        if (tag == kDeviceNameTag) {
            if (found_device_name || length != kDeviceNameSize) {
                return false;
            }
            found_device_name = true;
            const auto count = input[position++];
            if (count == 0U || count > Settings::kDeviceNameCapacity) {
                return false;
            }
            decoded.device_name_length = count;
            for (auto& character : decoded.device_name) {
                character = static_cast<char>(input[position++]);
            }
            if (std::string_view{decoded.device_name.data(), count}.find('\0') !=
                std::string_view::npos) {
                return false;
            }
            continue;
        }
        if (tag == kHostnameTag) {
            if (found_hostname || length != kHostnameSize) {
                return false;
            }
            found_hostname = true;
            const auto count = input[position++];
            if (count == 0U || count > Settings::kHostnameCapacity) {
                return false;
            }
            decoded.hostname_length = count;
            for (auto& character : decoded.hostname) {
                character = static_cast<char>(input[position++]);
            }
            if (std::string_view{decoded.hostname.data(), count}.find('\0') !=
                std::string_view::npos) {
                return false;
            }
            continue;
        }
        if (tag == kOtaUpdateTag) {
            if (found_ota_update || length != kOtaUpdateSize) {
                return false;
            }
            found_ota_update = true;
            const auto count = input[position++];
            if (count > Settings::kOtaUrlCapacity) {
                return false;
            }
            decoded.ota_url_length = count;
            for (auto& character : decoded.ota_url) {
                character = static_cast<char>(input[position++]);
            }
            if (std::string_view{decoded.ota_url.data(), count}.find('\0') !=
                std::string_view::npos) {
                return false;
            }
            const auto pending = input[position++];
            if (pending > 1U) {
                return false;
            }
            decoded.ota_update_pending = pending != 0U;
            continue;
        }
        if (tag == kLightTag) {
            if (found_light)
                return false;
            found_light = true;
            if (length != kLightV1Size && length != kLightV2Size) {
                // A record of another size reads as missing: Light starts from its defaults.
                position += length;
                continue;
            }
            auto& light = decoded.light;
            light.frequency = read_u32(input + position);
            position += 4;
            light.range = read_u16(input + position);
            position += 2;
            for (auto& duty : light.raw) {
                duty = read_u16(input + position);
                position += 2;
            }
            for (auto& value : light.channels)
                value = input[position++];
            light.color_temperature = read_u16(input + position);
            position += 2;
            light.dimmer = input[position++];
            light.speed = input[position++];
            light.desired_power = input[position++];
            const auto gamma = input[position++];
            const auto fade = input[position++];
            if (gamma > 1U || fade > 1U)
                return false;
            light.gamma = gamma != 0U;
            light.fade = fade != 0U;
            const auto extension_version = input[position++];
            if ((length == kLightV1Size && extension_version != 1U) ||
                (length == kLightV2Size && extension_version != 2U))
                return false;
            for (auto& hz : light.channel_frequency) {
                hz = read_u32(input + position);
                position += 4;
            }
            for (auto& entry : light.palette)
                for (auto& channel : entry)
                    channel = input[position++];
            light.palette_count = input[position++];
            light.palette_index = input[position++];
            light.fixed_color_index = input[position++];
            light.scheme = input[position++];
            light.white_dimmer = input[position++];
            light.wakeup_duration = read_u16(input + position);
            position += 2;
            if (length == kLightV2Size) {
#if USE_WS2812
                light.strip_pixels = read_u16(input + position);
                position += 2;
                light.strip_rotation = read_u16(input + position);
                position += 2;
                light.strip_step_pixels = read_u16(input + position);
                position += 2;
                for (auto& width : light.strip_width)
                    width = input[position++];
                light.strip_height = read_u16(input + position);
                position += 2;
                light.strip_type = input[position++];
                const auto reverse = input[position++];
                const auto alternate = input[position++];
                if (reverse > 1U || alternate > 1U)
                    return false;
                light.strip_reverse = reverse != 0U;
                light.strip_alternate = alternate != 0U;
                for (auto& color : light.strip_clock_colors)
                    for (auto& channel : color)
                        channel = input[position++];
#else
                std::copy_n(
                    input + position, kLightV2ExtensionSize, light.strip_extension_raw.begin());
                position += kLightV2ExtensionSize;
#endif
                light.strip_extension_present = true;
            }
#if !USE_WS2812
            // Retain strip extension data across firmware changes, but start PWM
            // Lights in a static state when the saved strip effect is unavailable.
            if (light.scheme >= 5U && light.scheme <= 14U)
                light.scheme = 0U;
#endif
#if USE_PWM || USE_WS2812
            if (!light.valid())
                return false;
#endif
            continue;
        }
        if (tag == kHardwareTag) {
            if (found_hardware || length != kHardwareSize) {
                return false;
            }
            found_hardware = true;
            auto& hardware = decoded.hardware;
            hardware.architecture_length = input[position++];
            for (auto& character : hardware.architecture_storage) {
                character = static_cast<char>(input[position++]);
            }
            hardware.module = input[position++];
            hardware.user_template_name_length = input[position++];
            for (auto& character : hardware.user_template_name_storage) {
                character = static_cast<char>(input[position++]);
            }
            hardware.user_template_base = input[position++];
            hardware.user_template_flag = read_u32(input + position);
            position += 4;
            hardware.user_template_size = input[position++];
            for (auto& assignment : hardware.user_template) {
                assignment = read_u16(input + position);
                position += 2;
            }
            hardware.override_count = input[position++];
            for (auto& override : hardware.overrides) {
                override.pin = PinId{read_u16(input + position)};
                override.function = read_u16(input + position + 2);
                position += 4;
            }
            if (!valid_hardware_shape(hardware)) {
                return false;
            }
            continue;
        }
        /** This record remains known in Safeboot/excluded builds so saves cannot discard it. */
        if (tag == kStateTextTag) {
            if (found_state_text || length != kStateTextSize)
                return false;
            found_state_text = true;
            for (auto& alias : decoded.state_text) {
                bool terminated = false;
                for (auto& character : alias) {
                    const auto byte = input[position++];
                    if (!byte)
                        terminated = true;
                    else if (terminated || byte < 32 || byte == 127 || byte == ' ')
                        return false;
                    character = static_cast<char>(byte);
                }
                if (!terminated)
                    return false;
            }
            continue;
        }
        if (tag == kGlobalTemperatureTag) {
            if (found_global_temperature || length != kGlobalTemperatureSize ||
                input[position] > 250)
                return false;
            found_global_temperature = true;
            decoded.global_temperature_source = input[position++];
            continue;
        }
        if (tag == kSleepTag) {
            if (found_sleep || length != kSleepSize || input[position] > Settings::kMaximumSleep)
                return false;
            found_sleep = true;
            decoded.sleep = input[position++];
            continue;
        }
        if (tag == kWifiPowerTag) {
            if (found_wifi_power || length != kWifiPowerSize)
                return false;
            found_wifi_power = true;
            decoded.wifi_power = input[position++];
            continue;
        }
        if (tag == kButtonDebounceTag) {
            if (found_button_debounce || length != kButtonDebounceSize)
                return false;
            const auto value = read_u16(input + position);
            if (!Settings::valid_button_debounce(value))
                return false;
            found_button_debounce = true;
            decoded.button_debounce = value;
            position += kButtonDebounceSize;
            continue;
        }
        if (tag == kButtonMqttTag) {
            if (found_button_mqtt || length != kButtonMqttSize)
                return false;
            found_button_mqtt = true;
            auto& mqtt = decoded.mqtt;
            mqtt.button_topic_length = input[position++];
            const auto retain = input[position++];
            if (retain > 1U)
                return false;
            mqtt.button_retain = retain != 0U;
            for (auto& character : mqtt.button_topic_storage)
                character = static_cast<char>(input[position++]);
            if (!mqtt.valid())
                return false;
            continue;
        }
        if (tag == kRgbwwTableTag) {
            if (found_rgbww_table || length != kRgbwwTableSize)
                return false;
            found_rgbww_table = true;
            for (auto& value : decoded.light.rgbww_table)
                value = input[position++];
            continue;
        }
        if (tag == kLedCurrentTag) {
            if (found_led_current || length != kLedCurrentSize)
                return false;
            found_led_current = true;
            for (auto& value : decoded.light.led_current)
                value = input[position++];
            continue;
        }
        if (tag == kSwitchTag) {
            if (found_switch || length != kSwitchSize)
                return false;
            const auto debounce = read_u16(input + position);
            if (!Settings::valid_button_debounce(debounce))
                return false;
            found_switch = true;
            decoded.switch_debounce = debounce;
            position += 2;
            for (auto& mode : decoded.switch_modes) {
                mode = input[position++];
                if (mode >= Settings::kSwitchModeCount)
                    return false;
            }
            continue;
        }
        if (tag == kSwitchMqttTag) {
            if (found_switch_mqtt || length != kSwitchMqttSize)
                return false;
            found_switch_mqtt = true;
            auto& mqtt = decoded.mqtt;
            mqtt.switch_topic_length = input[position++];
            const auto retain = input[position++];
            if (retain > 1U)
                return false;
            mqtt.switch_retain = retain != 0U;
            for (auto& character : mqtt.switch_topic_storage)
                character = static_cast<char>(input[position++]);
            if (!mqtt.valid())
                return false;
            continue;
        }
        if (tag == kTuyaTag) {
            if (found_tuya || length != kTuyaSize)
                return false;
            found_tuya = true;
            for (auto& entry : decoded.tuya.functions) {
                entry.function = input[position++];
                entry.dp = input[position++];
            }
            const auto stat_topic = input[position++];
            if (stat_topic > 1U)
                return false;
            decoded.tuya.stat_topic = stat_topic != 0U;
            decoded.tuya.dimmer_min = read_u16(input + position);
            decoded.tuya.dimmer_max = read_u16(input + position + 2);
            position += 4;
            decoded.tuya.rgb_type = input[position++];
            if (decoded.tuya.rgb_type > 3U)
                return false;
            decoded.tuya.temp_set_res = input[position++];
            if (decoded.tuya.temp_set_res > 3U)
                return false;
            for (auto& maximum : decoded.tuya.enum_max) {
                maximum = input[position++];
                if (maximum > 31U)
                    return false;
            }
            continue;
        }
        if (tag == kSwitchTextTag) {
            if (found_switch_text || length != kSwitchTextSize)
                return false;
            found_switch_text = true;
            for (auto& text : decoded.switch_texts) {
                bool terminated = false;
                for (auto& character : text) {
                    const auto byte = input[position++];
                    if (!byte)
                        terminated = true;
                    else if (terminated || byte <= ' ' || byte == 127 || byte == '"' ||
                             byte == '\\')
                        return false;
                    character = static_cast<char>(byte);
                }
                if (!terminated)
                    return false;
            }
            continue;
        }
        if (tag == kEnergyOutputTag) {
            if (found_energy_output)
                return false;
            found_energy_output = true;
            if (length != kEnergyOutputSize) {
                // A record of another size reads as missing: the resolutions keep defaults.
                position += length;
                continue;
            }
            // Decode/encode stays unconditional so excluded features preserve every choice.
            for (auto& decimals : decoded.energy_output.decimals)
                decimals = input[position++];
            if (!decoded.energy_output.valid())
                return false;
            continue;
        }
        if (tag == kPowerTag) {
            if (found_power)
                return false;
            found_power = true;
            if (length != kPowerSize) {
                // A record of another size reads as missing: power policy keeps defaults.
                position += length;
                continue;
            }
            decoded.power.save_data_seconds = read_u16(input + position);
            position += 2;
            decoded.power.power_on_state = input[position++];
            decoded.power.saved_state = read_u32(input + position);
            position += 4;
            if (decoded.power.save_data_seconds > 3600U || decoded.power.power_on_state > 5U) {
                return false;
            }
            for (auto& pulse_time : decoded.power.pulse_times) {
                pulse_time = read_u16(input + position);
                position += 2;
            }
            decoded.power.blink_time = read_u16(input + position);
            position += 2;
            decoded.power.blink_count = read_u16(input + position);
            position += 2;
            const auto interlock_enabled = input[position++];
            if (decoded.power.blink_time < 2U || decoded.power.blink_time > 3600U ||
                decoded.power.blink_count > 32000U || interlock_enabled > 1U) {
                return false;
            }
            decoded.power.interlock_enabled = interlock_enabled != 0U;
            for (auto& group : decoded.power.interlock_groups) {
                group = read_u32(input + position);
                position += 4;
            }
            if (!valid_interlock_groups(decoded.power.interlock_groups)) {
                return false;
            }
            decoded.power.led_state = input[position++];
            decoded.power.led_mask = read_u16(input + position);
            position += 2;
            if (decoded.power.led_state > 8U) {
                return false;
            }
            decoded.power.led_pwm_mask = input[position++];
            decoded.power.led_pwm_on = input[position++];
            decoded.power.led_pwm_off = input[position++];
            if (decoded.power.led_pwm_mask > 0x0FU) {
                return false;
            }
            continue;
        }
        unknown_record = true;
        position += length;
    }
    if (!decoded.mqtt.valid())
        return false;
    if (!found_set_options) {
        return false;
    }
    generation = read_u32(input + 8);
    writable = !unknown_record;
    wifi_present = found_wifi;
    return true;
}

/**
 * @brief Decodes settings slots into the transaction buffer and keeps the newest record.
 */
class SettingsRecordReader final : public RecordSlotReader {
  public:
    SettingsRecordReader(Settings::Data& candidate, Settings::Data& newest)
        : candidate_(candidate), newest_(newest) {}

    bool check(const std::uint8_t slot, const std::uint8_t* const bytes, const std::size_t size,
        std::uint32_t& generation) override {
        return decode(bytes, size, candidate_, generation, writable_[slot], wifi_present_[slot]);
    }

    void keep(const std::uint8_t slot, const std::uint8_t* const, const std::size_t) override {
        newest_ = candidate_;
        newest_wifi_present_ = wifi_present_[slot];
    }

    /** @brief Whether a decoded slot holds only fields this image knows. */
    [[nodiscard]] bool writable(const std::uint8_t slot) const { return writable_[slot]; }

    /** @brief Whether the kept record carried Wi-Fi fields. */
    [[nodiscard]] bool newest_wifi_present() const { return newest_wifi_present_; }

  private:
    Settings::Data& candidate_;
    Settings::Data& newest_;
    std::array<bool, 2> writable_{};
    std::array<bool, 2> wifi_present_{};
    bool newest_wifi_present_{false};
};

/** @brief Reads back and validates the exact critical record just written. */
[[nodiscard]] bool verify_critical_encoded(
    const std::array<std::uint8_t, kCriticalEncodedSize>& expected) {
    auto& actual = critical_read_scratch;
    std::size_t size = 0;
    if (platform::read_critical_settings(actual.data(), actual.size(), size) !=
            platform::CriticalSettingsResult::succeeded ||
        size != expected.size()) {
        return false;
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (actual[index] != expected[index]) {
            return false;
        }
    }
    return true;
}

} // namespace

/** @brief Constructs settings with the current target's Generic Template. */
Settings::Settings() {
    initialize_default_data(data_);
}

/**
 * @brief Loads the valid settings slot with the newest generation.
 */
SettingsLoadResult Settings::load() {
    initialize_default_data(data_);
    telemetry_request_generation_ = 0U;
    light_dirty_ = false;
    generation_ = 0;
    critical_generation_ = 0;
    runtime_applied_ = false;
    pwm_restart_required_ = false;
    critical_read_only_ = false;
    suppress_power_save_until_restart_ = false;

    SettingsRecordReader reader(transaction_candidate, data_);
    const auto stored = kSlots.load(record_scratch.data(), record_scratch.size(), reader);
    active_slot_ = stored.slot;
    generation_ = stored.generation;
    // Never replace an unreadable newer generation with recovered/default data.
    // Protect both slots: either may contain fields introduced by another image.
    read_only_ = false;
    for (std::uint8_t slot = 0U; slot < stored.states.size(); ++slot) {
        if (stored.states[slot] != RecordSlotState::absent && !reader.writable(slot))
            read_only_ = true;
    }
    const bool selected_wifi_present = reader.newest_wifi_present();

    SettingsLoadResult result = SettingsLoadResult::defaults;
    if (stored.loaded()) {
        // An unreadable other slot may hold a newer generation than the one loaded.
        result = stored.clean() ? SettingsLoadResult::loaded : SettingsLoadResult::recovered;
    } else if (stored.any(RecordSlotState::unavailable)) {
        result = SettingsLoadResult::unavailable;
    } else if (stored.any(RecordSlotState::failed)) {
        result = SettingsLoadResult::failed;
    } else if (stored.any(RecordSlotState::invalid)) {
        result = SettingsLoadResult::corrupt;
    }

    if (platform::critical_settings_store_enabled()) {
        auto& encoded = critical_read_scratch;
        std::size_t size = 0;
        switch (platform::read_critical_settings(encoded.data(), encoded.size(), size)) {
        case platform::CriticalSettingsResult::succeeded: {
            bool writable = false;
            if (!decode_critical_wifi(encoded.data(), size, critical_wifi_candidate,
                    critical_generation_, writable)) {
                AddLog(LogLevel::error, "CFG: critical credential record failed validation");
                install_wifi(WifiFields{}, data_);
                critical_read_only_ = true;
                result = SettingsLoadResult::corrupt;
                break;
            }
            install_wifi(critical_wifi_candidate, data_);
            critical_read_only_ = !writable;
            if (!writable) {
                AddLog(LogLevel::error, "CFG: critical credential record has unsupported fields");
            }
            break;
        }
        case platform::CriticalSettingsResult::not_found:
            if (selected_wifi_present && !save_critical_wifi(data_, true)) {
                result = SettingsLoadResult::failed;
            }
            break;
        case platform::CriticalSettingsResult::unavailable:
            install_wifi(WifiFields{}, data_);
            critical_read_only_ = true;
            result = SettingsLoadResult::unavailable;
            break;
        case platform::CriticalSettingsResult::failed:
            install_wifi(WifiFields{}, data_);
            critical_read_only_ = true;
            result = SettingsLoadResult::failed;
            break;
        }
    }

#if USE_SENSOR_SETTINGS
    SensorSettings sensor_policy{};
    const auto sensor_result = read_sensor_settings(sensor_policy);
    if (sensor_result.loaded()) {
        data_.sensors = sensor_policy;
    } else if (!sensor_result.clean()) {
        // A corrupt policy must not resume probes on a potentially conflicting address.
        data_.sensors.disabled.fill(0xffU);
    }
#endif

    save_data_counter_ = data_.power.save_data_seconds;
    return result;
}

/** @brief Captures the hardware and startup inputs used by the running image. */
void Settings::mark_runtime_applied() {
    applied_hardware_ = data_.hardware;
    applied_ssids_ = data_.ssids;
    applied_ssid_lengths_ = data_.ssid_lengths;
    applied_passwords_ = data_.passwords;
    applied_password_lengths_ = data_.password_lengths;
    applied_preferred_wifi_slot_ = data_.preferred_wifi_slot;
    (void)option(153U, applied_set_option_153_);
    (void)option(15U, applied_set_option_15_);
    (void)option(68U, applied_set_option_68_);
    (void)option(37U, applied_set_option_37_);
    runtime_applied_ = true;
    pwm_restart_required_ = false;
}

/** @brief Compares persistent restart-sensitive inputs with the active image. */
bool Settings::restart_required() const {
    if (!runtime_applied_) {
        return false;
    }
    if (pwm_restart_required_)
        return true;
    if (!equal_hardware_configuration(data_.hardware, applied_hardware_) ||
        data_.ssids != applied_ssids_ || data_.ssid_lengths != applied_ssid_lengths_ ||
        data_.passwords != applied_passwords_ ||
        data_.password_lengths != applied_password_lengths_ ||
        data_.preferred_wifi_slot != applied_preferred_wifi_slot_) {
        return true;
    }
    std::uint8_t current = 0U;
    if (option(15U, current) && current != applied_set_option_15_)
        return true;
    if (option(68U, current) && current != applied_set_option_68_)
        return true;
    if (option(37U, current) && current != applied_set_option_37_)
        return true;
    return option(153U, current) && current != applied_set_option_153_;
}

/**
 * @brief Identifies byte-valued SetOption indices.
 */
bool Settings::is_parameter_option(const std::uint16_t index) {
    return index >= 32 && index <= 49;
}

/**
 * @brief Reads one boolean or byte-valued SetOption.
 */
bool Settings::option(const std::uint16_t index, std::uint8_t& value) const {
    if (index >= kOptionCount) {
        return false;
    }
    switch (index) {
    case 3U:
        value = data_.mqtt.enabled;
        return true;
    case 4U:
        value = data_.mqtt.response_topic;
        return true;
    case 10U:
        value = data_.mqtt.offline_lwt;
        return true;
    case 26U:
        value = data_.mqtt.power_index;
        return true;
    case 75U:
        value = data_.mqtt.group_topic_mode;
        return true;
    case 90U:
        value = data_.mqtt.only_json;
        return true;
    case 104U:
        value = data_.mqtt.no_retain;
        return true;
    default:
        break;
    }
    if (is_parameter_option(index)) {
        value = data_.parameters[index - 32];
        return true;
    }

    const std::uint16_t compact_index = index < 32 ? index : index - 18;
    value =
        static_cast<std::uint8_t>((data_.flags[compact_index / 32] >> (compact_index % 32)) & 1U);
    return true;
}

/**
 * @brief Validates and transactionally persists one SetOption value.
 */
SettingsUpdateResult Settings::set_option(const std::uint16_t index, const std::uint8_t value) {
    if (index >= kOptionCount || (is_parameter_option(index) ? false : value > 1) ||
        (index == 32U && (value == 0U || value > 250U))) {
        return SettingsUpdateResult::invalid;
    }

    std::uint8_t current = 0;
    (void)option(index, current);
    if (current == value) {
        return SettingsUpdateResult::unchanged;
    }

    auto& candidate = begin_transaction(data_);
    switch (index) {
    case 3U:
        candidate.mqtt.enabled = value != 0U;
        break;
    case 4U:
        candidate.mqtt.response_topic = value != 0U;
        break;
    case 10U:
        candidate.mqtt.offline_lwt = value != 0U;
        break;
    case 26U:
        candidate.mqtt.power_index = value != 0U;
        break;
    case 75U:
        candidate.mqtt.group_topic_mode = value != 0U;
        break;
    case 90U:
        candidate.mqtt.only_json = value != 0U;
        if (value)
            candidate.mqtt.power_retain = false;
        break;
    case 104U:
        candidate.mqtt.no_retain = value != 0U;
        break;
    default:
        break;
    }
    if (!candidate.mqtt.valid())
        return SettingsUpdateResult::invalid;
    if (is_parameter_option(index)) {
        candidate.parameters[index - 32] = value;
    } else {
        const std::uint16_t compact_index = index < 32 ? index : index - 18;
        const std::uint32_t mask = 1U << (compact_index % 32);
        auto& flags = candidate.flags[compact_index / 32];
        flags = value == 0 ? flags & ~mask : flags | mask;
        if (index == 0U && value == 0U) {
            candidate.power.saved_state = 0U;
        }
    }

    if (!save(candidate)) {
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/**
 * @brief Returns the SSID stored in a zero-based Wi-Fi slot.
 */
std::string_view Settings::ssid(const std::uint8_t slot) const {
    if (slot >= kWifiSlotCount) {
        return {};
    }
    return {data_.ssids[slot].data(), data_.ssid_lengths[slot]};
}

/**
 * @brief Returns the password stored in a zero-based Wi-Fi slot.
 */
std::string_view Settings::password(const std::uint8_t slot) const {
    if (slot >= kWifiSlotCount) {
        return {};
    }
    return {data_.passwords[slot].data(), data_.password_lengths[slot]};
}

/**
 * @brief Validates and transactionally persists one SSID.
 */
SettingsUpdateResult Settings::set_ssid(
    const std::uint8_t slot, const std::string_view value, const bool select) {
    if (slot >= kWifiSlotCount || value.size() > kSsidCapacity ||
        value.find('\0') != std::string_view::npos) {
        return SettingsUpdateResult::invalid;
    }
    if (ssid(slot) == value && (!select || data_.preferred_wifi_slot == slot)) {
        return SettingsUpdateResult::unchanged;
    }

    auto& candidate = begin_transaction(data_);
    candidate.ssids[slot] = {};
    for (std::size_t index = 0; index < value.size(); ++index) {
        candidate.ssids[slot][index] = value[index];
    }
    candidate.ssid_lengths[slot] = static_cast<std::uint8_t>(value.size());
    if (select) {
        candidate.preferred_wifi_slot = slot;
    }
    if (platform::critical_settings_store_enabled() ? !save_critical_wifi(candidate)
                                                    : !save(candidate)) {
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/**
 * @brief Validates and transactionally persists one Wi-Fi password.
 */
SettingsUpdateResult Settings::set_password(
    const std::uint8_t slot, const std::string_view value, const bool select) {
    if (slot >= kWifiSlotCount || value.size() > kPasswordCapacity ||
        value.find('\0') != std::string_view::npos) {
        return SettingsUpdateResult::invalid;
    }
    if (password(slot) == value && (!select || data_.preferred_wifi_slot == slot)) {
        return SettingsUpdateResult::unchanged;
    }

    auto& candidate = begin_transaction(data_);
    candidate.passwords[slot] = {};
    for (std::size_t index = 0; index < value.size(); ++index) {
        candidate.passwords[slot][index] = value[index];
    }
    candidate.password_lengths[slot] = static_cast<std::uint8_t>(value.size());
    if (select) {
        candidate.preferred_wifi_slot = slot;
    }
    if (platform::critical_settings_store_enabled() ? !save_critical_wifi(candidate)
                                                    : !save(candidate)) {
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/**
 * @brief Validates and transactionally persists station and web credentials.
 */
SettingsUpdateResult Settings::set_wifi_configuration(const std::uint8_t slot,
    const std::string_view ssid_value, const std::string_view password_value,
    const std::string_view web_password_value, const bool select) {
    if (slot >= kWifiSlotCount || ssid_value.size() > kSsidCapacity ||
        password_value.size() > kPasswordCapacity ||
        web_password_value.size() > kPasswordCapacity ||
        (!web_password_value.empty() && web_password_value.size() < 8U) ||
        ssid_value.find('\0') != std::string_view::npos ||
        password_value.find('\0') != std::string_view::npos ||
        web_password_value.find('\0') != std::string_view::npos) {
        return SettingsUpdateResult::invalid;
    }
    if (ssid(slot) == ssid_value && password(slot) == password_value &&
        web_password() == web_password_value && (!select || data_.preferred_wifi_slot == slot)) {
        return SettingsUpdateResult::unchanged;
    }

    auto& candidate = begin_transaction(data_);
    candidate.ssids[slot] = {};
    for (std::size_t index = 0; index < ssid_value.size(); ++index) {
        candidate.ssids[slot][index] = ssid_value[index];
    }
    candidate.ssid_lengths[slot] = static_cast<std::uint8_t>(ssid_value.size());
    candidate.passwords[slot] = {};
    for (std::size_t index = 0; index < password_value.size(); ++index) {
        candidate.passwords[slot][index] = password_value[index];
    }
    candidate.password_lengths[slot] = static_cast<std::uint8_t>(password_value.size());
    candidate.web_password = {};
    for (std::size_t index = 0; index < web_password_value.size(); ++index) {
        candidate.web_password[index] = web_password_value[index];
    }
    candidate.web_password_length = static_cast<std::uint8_t>(web_password_value.size());
    if (select) {
        candidate.preferred_wifi_slot = slot;
    }

    // On adapters with the OTA-critical store, write the record containing
    // both Wi-Fi and web credentials first.  A subsequent normal-store
    // failure is reconciled by load(), which treats the critical record as
    // authoritative for this complete provisioning tuple.
    if (platform::critical_settings_store_enabled() && !save_critical_wifi(candidate)) {
        AddLog(LogLevel::error, critical_read_only_
                                    ? "CFG: credential save blocked; critical settings locked"
                                    : "CFG: critical credential write or verification failed");
        return SettingsUpdateResult::failed;
    }
    if (!save(candidate)) {
        AddLog(LogLevel::error, read_only()
                                    ? "CFG: credential save blocked; settings writes locked"
                                    : "CFG: credential save failed in LittleFS settings store");
        if (platform::critical_settings_store_enabled()) {
            (void)load();
        }
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/** @brief Replaces both station credential slots atomically. */
SettingsUpdateResult Settings::set_wifi_credentials(
    const std::array<std::string_view, kWifiSlotCount>& ssid_values,
    const std::array<std::string_view, kWifiSlotCount>& password_values,
    const std::string_view web_password_value, const std::uint8_t preferred_slot) {
    if (preferred_slot >= kWifiSlotCount || web_password_value.size() > kPasswordCapacity ||
        (!web_password_value.empty() && web_password_value.size() < 8U) ||
        web_password_value.find('\0') != std::string_view::npos) {
        return SettingsUpdateResult::invalid;
    }
    for (std::size_t slot = 0; slot < kWifiSlotCount; ++slot) {
        if (ssid_values[slot].size() > kSsidCapacity ||
            password_values[slot].size() > kPasswordCapacity ||
            ssid_values[slot].find('\0') != std::string_view::npos ||
            password_values[slot].find('\0') != std::string_view::npos) {
            return SettingsUpdateResult::invalid;
        }
    }

    bool changed =
        data_.preferred_wifi_slot != preferred_slot || web_password() != web_password_value;
    for (std::size_t slot = 0; slot < kWifiSlotCount; ++slot) {
        changed = changed || ssid(static_cast<std::uint8_t>(slot)) != ssid_values[slot] ||
                  password(static_cast<std::uint8_t>(slot)) != password_values[slot];
    }
    if (!changed) {
        return SettingsUpdateResult::unchanged;
    }

    auto& candidate = begin_transaction(data_);
    for (std::size_t slot = 0; slot < kWifiSlotCount; ++slot) {
        candidate.ssids[slot] = {};
        candidate.ssid_lengths[slot] = static_cast<std::uint8_t>(ssid_values[slot].size());
        for (std::size_t index = 0; index < ssid_values[slot].size(); ++index) {
            candidate.ssids[slot][index] = ssid_values[slot][index];
        }
        candidate.passwords[slot] = {};
        candidate.password_lengths[slot] = static_cast<std::uint8_t>(password_values[slot].size());
        for (std::size_t index = 0; index < password_values[slot].size(); ++index) {
            candidate.passwords[slot][index] = password_values[slot][index];
        }
    }
    candidate.preferred_wifi_slot = preferred_slot;
    candidate.web_password = {};
    for (std::size_t index = 0; index < web_password_value.size(); ++index) {
        candidate.web_password[index] = web_password_value[index];
    }
    candidate.web_password_length = static_cast<std::uint8_t>(web_password_value.size());

    if (platform::critical_settings_store_enabled() && !save_critical_wifi(candidate)) {
        return SettingsUpdateResult::failed;
    }
    if (!save(candidate)) {
        if (platform::critical_settings_store_enabled()) {
            (void)load();
        }
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/** @brief Persists the Wi-Fi slot selected for the next startup. */
SettingsUpdateResult Settings::set_preferred_wifi_slot(const std::uint8_t slot) {
    if (slot >= kWifiSlotCount) {
        return SettingsUpdateResult::invalid;
    }
    if (data_.preferred_wifi_slot == slot) {
        return SettingsUpdateResult::unchanged;
    }

    auto& candidate = begin_transaction(data_);
    candidate.preferred_wifi_slot = slot;
    if (platform::critical_settings_store_enabled() ? !save_critical_wifi(candidate)
                                                    : !save(candidate)) {
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/** @brief Returns the provisioned web credential without exposing it in status. */
std::string_view Settings::web_password() const {
    return {data_.web_password.data(), data_.web_password_length};
}

/** @brief Commits a bounded web credential through the normal settings transaction. */
SettingsUpdateResult Settings::set_web_password(const std::string_view value) {
    if (value.size() > kPasswordCapacity || (!value.empty() && value.size() < 8) ||
        value.find('\0') != std::string_view::npos) {
        return SettingsUpdateResult::invalid;
    }
    return set_wifi_configuration(data_.preferred_wifi_slot, ssid(data_.preferred_wifi_slot),
        password(data_.preferred_wifi_slot), value, false);
}

/** @brief Returns the persisted web/UI device label. */
std::string_view Settings::device_name() const {
    return {data_.device_name.data(), data_.device_name_length};
}

/** @brief Commits a bounded web/UI device label. */
SettingsUpdateResult Settings::set_device_name(const std::string_view value) {
    if (value.empty() || value.size() > kDeviceNameCapacity ||
        value.find('\0') != std::string_view::npos) {
        return SettingsUpdateResult::invalid;
    }
    if (device_name() == value) {
        return SettingsUpdateResult::unchanged;
    }

    auto& candidate = begin_transaction(data_);
    candidate.device_name.fill('\0');
    for (std::size_t index = 0U; index < value.size(); ++index) {
        candidate.device_name[index] = value[index];
    }
    candidate.device_name_length = static_cast<std::uint8_t>(value.size());
    if (!save(candidate)) {
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/** @brief Returns the persisted hostname template or literal. */
std::string_view Settings::hostname() const {
    return {data_.hostname.data(), data_.hostname_length};
}

/** @brief Commits a bounded hostname template or literal. */
SettingsUpdateResult Settings::set_hostname(const std::string_view value) {
    if (value.empty() || value.size() > kHostnameCapacity ||
        value.find('\0') != std::string_view::npos) {
        return SettingsUpdateResult::invalid;
    }
    if (hostname() == value) {
        return SettingsUpdateResult::unchanged;
    }

    auto& candidate = begin_transaction(data_);
    candidate.hostname.fill('\0');
    for (std::size_t index = 0U; index < value.size(); ++index) {
        candidate.hostname[index] = value[index];
    }
    candidate.hostname_length = static_cast<std::uint8_t>(value.size());
    if (!save(candidate)) {
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/** @brief Returns the configured URL without adding a terminator. */
std::string_view Settings::ota_url() const {
    return {data_.ota_url.data(), data_.ota_url_length};
}

/** @brief Persists one bounded URL string; URL syntax is checked on use. */
SettingsUpdateResult Settings::set_ota_url(const std::string_view value) {
    if (value.size() > kOtaUrlCapacity || value.find('\0') != std::string_view::npos) {
        return SettingsUpdateResult::invalid;
    }
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x21U || byte > 0x7eU) {
            return SettingsUpdateResult::invalid;
        }
    }
    if (ota_url() == value) {
        return SettingsUpdateResult::unchanged;
    }

    auto& candidate = begin_transaction(data_);
    candidate.ota_url.fill('\0');
    for (std::size_t index = 0U; index < value.size(); ++index) {
        candidate.ota_url[index] = value[index];
    }
    candidate.ota_url_length = static_cast<std::uint8_t>(value.size());
    if (!save(candidate)) {
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/** @brief Persists one pending update handoff flag. */
SettingsUpdateResult Settings::set_ota_update_pending(const bool pending) {
    if (data_.ota_update_pending == pending) {
        return SettingsUpdateResult::unchanged;
    }
    auto& candidate = begin_transaction(data_);
    candidate.ota_update_pending = pending;
    if (!save(candidate)) {
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/** @brief Commits civil timer configuration before changing live behavior. */
#if USE_TIMERS
SettingsUpdateResult Settings::set_timer_settings(const TimerSettings& value) {
    if (!value.valid())
        return SettingsUpdateResult::invalid;
    if (data_.timers == value)
        return SettingsUpdateResult::unchanged;
    auto& candidate = begin_transaction(data_);
    candidate.timers = value;
    if (!save(candidate))
        return SettingsUpdateResult::failed;
    data_ = candidate;
    return SettingsUpdateResult::saved;
}
#endif

/**
 * @brief Persists the selected Time command response format.
 */
SettingsUpdateResult Settings::set_time_format(const std::uint8_t format) {
    if (format > 3) {
        return SettingsUpdateResult::invalid;
    }
    if (data_.time_format == format) {
        return SettingsUpdateResult::unchanged;
    }

    auto& candidate = begin_transaction(data_);
    candidate.time_format = format;
    if (!save(candidate)) {
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/** @brief Returns a bounded NTP address independently of the active feature set. */
std::string_view Settings::ntp_server(const std::uint8_t slot) const {
    return slot < kNtpServerCount
               ? std::string_view{data_.ntp_servers[slot].data(), data_.ntp_server_lengths[slot]}
               : std::string_view{};
}

/** @brief Persists an NTP address before changing its live value. */
SettingsUpdateResult Settings::set_ntp_server(
    const std::uint8_t slot, const std::string_view value) {
    if (slot >= kNtpServerCount || !valid_ntp_server(value))
        return SettingsUpdateResult::invalid;
    if (ntp_server(slot) == value)
        return SettingsUpdateResult::unchanged;
    auto& candidate = begin_transaction(data_);
    candidate.ntp_servers[slot].fill('\0');
    std::copy(value.begin(), value.end(), candidate.ntp_servers[slot].begin());
    candidate.ntp_server_lengths[slot] = static_cast<std::uint8_t>(value.size());
    if (!save(candidate))
        return SettingsUpdateResult::failed;
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/**
 * @brief Persists a fixed UTC offset or automatic timezone selection.
 */
SettingsUpdateResult Settings::set_timezone(
    const std::int16_t offset_minutes, const bool automatic) {
    if (offset_minutes < -839 || offset_minutes > 899) {
        return SettingsUpdateResult::invalid;
    }
    if (data_.timezone_offset_minutes == offset_minutes && data_.timezone_automatic == automatic) {
        return SettingsUpdateResult::unchanged;
    }

    auto& candidate = begin_transaction(data_);
    candidate.timezone_offset_minutes = offset_minutes;
    candidate.timezone_automatic = automatic;
    if (!save(candidate)) {
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/** @brief Persists the Tasmota-compatible serial log threshold. */
SettingsUpdateResult Settings::set_serial_log_level(const std::uint8_t level) {
    if (level > kMaximumLogLevel) {
        return SettingsUpdateResult::invalid;
    }
    if (data_.serial_log_level == level) {
        return SettingsUpdateResult::unchanged;
    }

    auto& candidate = begin_transaction(data_);
    candidate.serial_log_level = level;
    if (!save(candidate)) {
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/** @brief Persists the Tasmota-compatible future web log threshold. */
SettingsUpdateResult Settings::set_web_log_level(const std::uint8_t level) {
    if (level > kMaximumLogLevel) {
        return SettingsUpdateResult::invalid;
    }
    if (data_.web_log_level == level) {
        return SettingsUpdateResult::unchanged;
    }

    auto& candidate = begin_transaction(data_);
    candidate.web_log_level = level;
    if (!save(candidate)) {
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

#if USE_MQTT
/** @brief Persists one validated MQTT broker and publication configuration. */
SettingsUpdateResult Settings::set_mqtt_settings(const MqttSettings& settings) {
    return set_mqtt_configuration(settings, data_.tele_period_seconds);
}

SettingsUpdateResult Settings::set_mqtt_configuration(
    const MqttSettings& settings, const std::uint16_t tele_period) {
    if (!settings.valid() || !TelemetrySettings::valid_period(tele_period))
        return SettingsUpdateResult::invalid;
    if (data_.mqtt == settings && data_.tele_period_seconds == tele_period)
        return SettingsUpdateResult::unchanged;

    auto& candidate = begin_transaction(data_);
    candidate.mqtt = settings;
    candidate.tele_period_seconds = tele_period;
    const std::uint16_t indexes[]{3U, 4U, 10U, 26U, 75U, 90U, 104U};
    const bool values[]{settings.enabled, settings.response_topic, settings.offline_lwt,
        settings.power_index, settings.group_topic_mode, settings.only_json, settings.no_retain};
    for (std::size_t i = 0; i < 7U; ++i) {
        const auto compact = indexes[i] < 32U ? indexes[i] : indexes[i] - 18U;
        const auto mask = 1U << (compact % 32U);
        auto& flags = candidate.flags[compact / 32U];
        flags = values[i] ? flags | mask : flags & ~mask;
    }
    if (!save(candidate))
        return SettingsUpdateResult::failed;
    data_ = candidate;
    return SettingsUpdateResult::saved;
}
#endif

#if USE_SENSOR_SETTINGS
/** @brief Combines persisted sensor output choices with common unit SetOptions. */
SensorPresentation Settings::sensor_presentation() const {
    const auto& output = data_.sensors.output;
    SensorPresentation presentation{};
    presentation.temperature_resolution = output.temperature_resolution;
    presentation.humidity_resolution = output.humidity_resolution;
    presentation.pressure_resolution = output.pressure_resolution;
    presentation.temperature_offset = output.temperature_offset_tenths;
    presentation.humidity_offset = output.humidity_offset_tenths;
    const auto& energy = data_.energy_output.decimals;
    presentation.energy_resolution = energy[0];
    presentation.wattage_resolution = energy[1];
    presentation.voltage_resolution = energy[2];
    presentation.current_resolution = energy[3];
    presentation.frequency_resolution = energy[4];
    std::uint8_t option_value = 0U;
    if (option(8U, option_value))
        presentation.fahrenheit = option_value != 0U;
    if (option(24U, option_value))
        presentation.pressure_conversion = option_value != 0U;
    if (option(139U, option_value))
        presentation.pressure_inch = option_value != 0U;
    return presentation;
}

/** @brief Persists the new output settings before making them active. */
SettingsUpdateResult Settings::set_sensor_output_settings(const SensorOutputSettings& settings) {
    if (settings.temperature_resolution > 3U || settings.humidity_resolution > 3U ||
        settings.pressure_resolution > 3U || settings.temperature_offset_tenths < -126 ||
        settings.temperature_offset_tenths > 126 || settings.humidity_offset_tenths < -100 ||
        settings.humidity_offset_tenths > 100)
        return SettingsUpdateResult::invalid;

    const auto& current = data_.sensors.output;
    if (current.temperature_resolution == settings.temperature_resolution &&
        current.humidity_resolution == settings.humidity_resolution &&
        current.pressure_resolution == settings.pressure_resolution &&
        current.temperature_offset_tenths == settings.temperature_offset_tenths &&
        current.humidity_offset_tenths == settings.humidity_offset_tenths)
        return SettingsUpdateResult::unchanged;

    auto candidate = data_.sensors;
    candidate.output = settings;
    if (!save_sensor_settings(candidate))
        return SettingsUpdateResult::failed;
    data_.sensors = candidate;
    return SettingsUpdateResult::saved;
}

/** @brief Persists one per-driver probe policy in app-owned storage. */
SettingsUpdateResult Settings::set_sensor_driver_enabled(std::uint8_t id, bool enabled) {
    if (id == 0 || id > SensorSettings::kMaximumDriverId)
        return SettingsUpdateResult::invalid;
    if (data_.sensors.enabled(id) == enabled)
        return SettingsUpdateResult::unchanged;
    auto candidate = data_.sensors;
    candidate.set_enabled(id, enabled);
    if (!save_sensor_settings(candidate))
        return SettingsUpdateResult::failed;
    data_.sensors = candidate;
    return SettingsUpdateResult::saved;
}
#endif

/** @brief Resolves empty retained aliases and invalid indexes using source defaults. */
std::string_view Settings::state_text(unsigned index) const {
    constexpr std::string_view defaults[]{"OFF", "ON", "TOGGLE", "HOLD"};
    if (index >= 4)
        index = 1;
    const auto& alias = data_.state_text[index];
    if (!alias[0])
        return defaults[index];
    return {alias.data(), std::char_traits<char>::length(alias.data())};
}

SettingsUpdateResult Settings::set_state_text(unsigned index, std::string_view value) {
    if (index >= 4 || value.empty() || value.size() > 31)
        return SettingsUpdateResult::invalid;
    std::array<char, 32> alias{};
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto byte = static_cast<unsigned char>(value[i]);
        if (byte < 32 || byte == 127)
            return SettingsUpdateResult::invalid;
        alias[i] = byte == ' ' ? '_' : value[i];
    }
    if (alias == data_.state_text[index])
        return SettingsUpdateResult::unchanged;
    auto& candidate = begin_transaction(data_);
    candidate.state_text[index] = alias;
    if (!save(candidate))
        return SettingsUpdateResult::failed;
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

SettingsUpdateResult Settings::set_sleep(const std::uint8_t milliseconds) {
    if (milliseconds > kMaximumSleep)
        return SettingsUpdateResult::invalid;
    if (data_.sleep == milliseconds)
        return SettingsUpdateResult::unchanged;
    auto& candidate = begin_transaction(data_);
    candidate.sleep = milliseconds;
    if (!save(candidate))
        return SettingsUpdateResult::failed;
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/** @brief Preserves the committed filter on invalid input or a failed transaction. */
SettingsUpdateResult Settings::set_button_debounce(const std::uint16_t milliseconds) {
    if (!valid_button_debounce(milliseconds))
        return SettingsUpdateResult::invalid;
    if (data_.button_debounce == milliseconds)
        return SettingsUpdateResult::unchanged;
    auto& candidate = begin_transaction(data_);
    candidate.button_debounce = milliseconds;
    if (!save(candidate))
        return SettingsUpdateResult::failed;
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/** @brief Preserves the committed switch filter on invalid input or a failed transaction. */
SettingsUpdateResult Settings::set_switch_debounce(const std::uint16_t milliseconds) {
    if (!valid_button_debounce(milliseconds))
        return SettingsUpdateResult::invalid;
    if (data_.switch_debounce == milliseconds)
        return SettingsUpdateResult::unchanged;
    auto& candidate = begin_transaction(data_);
    candidate.switch_debounce = milliseconds;
    if (!save(candidate))
        return SettingsUpdateResult::failed;
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

SettingsUpdateResult Settings::set_switch_mode(const std::size_t index, const std::uint8_t mode) {
    if (index > kSwitchCount || mode >= kSwitchModeCount)
        return SettingsUpdateResult::invalid;
    auto modes = data_.switch_modes;
    if (index == 0U)
        modes.fill(mode);
    else
        modes[index - 1U] = mode;
    if (modes == data_.switch_modes)
        return SettingsUpdateResult::unchanged;
    auto& candidate = begin_transaction(data_);
    candidate.switch_modes = modes;
    if (!save(candidate))
        return SettingsUpdateResult::failed;
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

std::string_view Settings::switch_text(const std::size_t index) const {
    if (index == 0U || index > kSwitchTextCount)
        return {};
    const auto& text = data_.switch_texts[index - 1U];
    return {text.data(), std::char_traits<char>::length(text.data())};
}

SettingsUpdateResult Settings::set_switch_text(const std::size_t index, std::string_view value) {
    if (index == 0U || index > kSwitchTextCount || value.size() >= kSwitchTextCapacity)
        return SettingsUpdateResult::invalid;
    std::array<char, kSwitchTextCapacity> text{};
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto byte = static_cast<unsigned char>(value[i]);
        if (byte <= ' ' || byte == 127 || byte == '"' || byte == '\\')
            return SettingsUpdateResult::invalid;
        text[i] = value[i];
    }
    if (text == data_.switch_texts[index - 1U])
        return SettingsUpdateResult::unchanged;
    auto& candidate = begin_transaction(data_);
    candidate.switch_texts[index - 1U] = text;
    if (!save(candidate))
        return SettingsUpdateResult::failed;
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

SettingsUpdateResult Settings::set_tuya_settings(const TuyaSettings& value) {
    if (value == data_.tuya)
        return SettingsUpdateResult::unchanged;
    auto& candidate = begin_transaction(data_);
    candidate.tuya = value;
    if (!save(candidate))
        return SettingsUpdateResult::failed;
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

SettingsUpdateResult Settings::set_wifi_power(const std::uint8_t tenths_dbm) {
    if (data_.wifi_power == tenths_dbm)
        return SettingsUpdateResult::unchanged;
    auto& candidate = begin_transaction(data_);
    candidate.wifi_power = tenths_dbm;
    if (!save(candidate))
        return SettingsUpdateResult::failed;
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

SettingsUpdateResult Settings::set_global_temperature_source(std::uint8_t source) {
    if (source > 250)
        return SettingsUpdateResult::invalid;
    if (data_.global_temperature_source == source)
        return SettingsUpdateResult::unchanged;
    auto& candidate = begin_transaction(data_);
    candidate.global_temperature_source = source;
    if (!save(candidate))
        return SettingsUpdateResult::failed;
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/** @brief Saves the retained formatting record before publishing its new runtime value. */
SettingsUpdateResult Settings::set_energy_output_settings(const energy::OutputSettings& settings) {
    if (!settings.valid())
        return SettingsUpdateResult::invalid;
    if (data_.energy_output.decimals == settings.decimals)
        return SettingsUpdateResult::unchanged;
    auto& candidate = begin_transaction(data_);
    candidate.energy_output = settings;
    if (!save(candidate))
        return SettingsUpdateResult::failed;
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/** @brief Persists the shared Tasmota-compatible telemetry interval. */
SettingsUpdateResult Settings::set_tele_period_seconds(const std::uint16_t seconds) {
    if (!TelemetrySettings::valid_period(seconds)) {
        return SettingsUpdateResult::invalid;
    }
    if (data_.tele_period_seconds == seconds) {
        return SettingsUpdateResult::unchanged;
    }

    auto& candidate = begin_transaction(data_);
    candidate.tele_period_seconds = seconds;
    if (!save(candidate))
        return SettingsUpdateResult::failed;
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/**
 * @brief Persists one validated standard or daylight transition rule.
 */
SettingsUpdateResult Settings::set_time_rule(const std::uint8_t index, const TimeRule& rule) {
    if (index >= kTimeRuleCount || !valid_time_rule(rule)) {
        return SettingsUpdateResult::invalid;
    }
    if (equal_time_rule(data_.time_rules[index], rule)) {
        return SettingsUpdateResult::unchanged;
    }

    auto& candidate = begin_transaction(data_);
    candidate.time_rules[index] = rule;
    if (!save(candidate)) {
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/** @brief Persists SaveData and snapshots current relay state in one write. */
SettingsUpdateResult Settings::set_save_data(
    const std::uint16_t seconds, const std::uint32_t current_power) {
    if (seconds > 3600U) {
        return SettingsUpdateResult::invalid;
    }
    std::uint8_t save_state = 0U;
    (void)option(0U, save_state);
    const std::uint32_t saved_power = save_state != 0U ? current_power : 0U;
    if (data_.power.save_data_seconds == seconds && data_.power.saved_state == saved_power) {
        save_data_counter_ = seconds;
        return SettingsUpdateResult::unchanged;
    }

    auto& candidate = begin_transaction(data_);
    candidate.power.save_data_seconds = seconds;
    candidate.power.saved_state = saved_power;
    if (!save(candidate)) {
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    save_data_counter_ = seconds;
    return SettingsUpdateResult::saved;
}

/** @brief Persists one of Tasmota's six PowerOnState values. */
SettingsUpdateResult Settings::set_power_on_state(const std::uint8_t value) {
    if (value > 5U) {
        return SettingsUpdateResult::invalid;
    }
    if (data_.power.power_on_state == value) {
        return SettingsUpdateResult::unchanged;
    }

    auto& candidate = begin_transaction(data_);
    candidate.power.power_on_state = value;
    if (!save(candidate)) {
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/** @brief Persists one zero-based Tasmota pulse timer value. */
SettingsUpdateResult Settings::set_pulse_time(const std::uint8_t index, const std::uint16_t value) {
    if (index >= data_.power.pulse_times.size()) {
        return SettingsUpdateResult::invalid;
    }
    if (data_.power.pulse_times[index] == value) {
        return SettingsUpdateResult::unchanged;
    }

    auto& candidate = begin_transaction(data_);
    candidate.power.pulse_times[index] = value;
    if (!save(candidate)) {
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/** @brief Persists a valid blink half-period in tenths of a second. */
SettingsUpdateResult Settings::set_led_state(const std::uint8_t value) {
    if (value > 8U) {
        return SettingsUpdateResult::invalid;
    }
    if (data_.power.led_state == value) {
        return SettingsUpdateResult::unchanged;
    }
    auto& candidate = begin_transaction(data_);
    candidate.power.led_state = value;
    if (!save(candidate)) {
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

SettingsUpdateResult Settings::set_led_mask(const std::uint16_t value) {
    if (data_.power.led_mask == value) {
        return SettingsUpdateResult::unchanged;
    }
    auto& candidate = begin_transaction(data_);
    candidate.power.led_mask = value;
    if (!save(candidate)) {
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

SettingsUpdateResult Settings::set_led_pwm(
    const std::uint8_t mask, const std::uint8_t on_level, const std::uint8_t off_level) {
    if (mask > 0x0FU) {
        return SettingsUpdateResult::invalid;
    }
    if (data_.power.led_pwm_mask == mask && data_.power.led_pwm_on == on_level &&
        data_.power.led_pwm_off == off_level) {
        return SettingsUpdateResult::unchanged;
    }
    auto& candidate = begin_transaction(data_);
    candidate.power.led_pwm_mask = mask;
    candidate.power.led_pwm_on = on_level;
    candidate.power.led_pwm_off = off_level;
    if (!save(candidate)) {
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

SettingsUpdateResult Settings::set_blink_time(const std::uint16_t value) {
    if (value < 2U || value > 3600U) {
        return SettingsUpdateResult::invalid;
    }
    if (data_.power.blink_time == value) {
        return SettingsUpdateResult::unchanged;
    }

    auto& candidate = begin_transaction(data_);
    candidate.power.blink_time = value;
    if (!save(candidate)) {
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/** @brief Persists a finite blink count or zero for continuous blinking. */
SettingsUpdateResult Settings::set_blink_count(const std::uint16_t value) {
    if (value > 32000U) {
        return SettingsUpdateResult::invalid;
    }
    if (data_.power.blink_count == value) {
        return SettingsUpdateResult::unchanged;
    }

    auto& candidate = begin_transaction(data_);
    candidate.power.blink_count = value;
    if (!save(candidate)) {
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/** @brief Persists validated interlock enablement and disjoint relay groups. */
SettingsUpdateResult Settings::set_interlock(
    const bool enabled, const std::array<std::uint32_t, kMaximumInterlockGroups>& groups) {
    if (!valid_interlock_groups(groups)) {
        return SettingsUpdateResult::invalid;
    }
    if (data_.power.interlock_enabled == enabled && data_.power.interlock_groups == groups) {
        return SettingsUpdateResult::unchanged;
    }

    auto& candidate = begin_transaction(data_);
    candidate.power.interlock_enabled = enabled;
    candidate.power.interlock_groups = groups;
    if (!save(candidate)) {
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/** @brief Saves relay state when the configured periodic deadline is due. */
SettingsUpdateResult Settings::poll_power_state(const std::uint32_t current_power) {
    if (suppress_power_save_until_restart_ || data_.power.save_data_seconds == 0U) {
        return SettingsUpdateResult::unchanged;
    }
    if (save_data_counter_ > 1U) {
        --save_data_counter_;
        return SettingsUpdateResult::unchanged;
    }
    save_data_counter_ = data_.power.save_data_seconds;
    return persist_power_state(current_power);
}

/** @brief Saves relay state immediately before an orderly restart. */
SettingsUpdateResult Settings::flush_power_state(const std::uint32_t current_power) {
    if (suppress_power_save_until_restart_) {
        return SettingsUpdateResult::unchanged;
    }
    return persist_power_state(current_power);
}

/** @brief Persists a changed relay mask according to SetOption0. */
SettingsUpdateResult Settings::persist_power_state(const std::uint32_t current_power) {
    std::uint8_t save_state = 0U;
    (void)option(0U, save_state);
    const std::uint32_t saved_power = save_state != 0U ? current_power : 0U;
    if (data_.power.saved_state == saved_power && !light_dirty_) {
        return SettingsUpdateResult::unchanged;
    }

    auto& candidate = begin_transaction(data_);
    candidate.power.saved_state = saved_power;
    if (!save(candidate)) {
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/** @brief Strictly validates and persists one complete hardware transaction. */
SettingsUpdateResult Settings::set_hardware_configuration(
    const HardwareConfiguration& configuration) {
    auto& candidate = begin_transaction(data_);
    auto& canonical = candidate.hardware;
    canonical = configuration;
    if (!valid_hardware_candidate(canonical)) {
        return SettingsUpdateResult::invalid;
    }
    canonicalize_hardware(canonical);
    if (equal_hardware_configuration(data_.hardware, canonical)) {
        return SettingsUpdateResult::unchanged;
    }

    if (!save(candidate)) {
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    return SettingsUpdateResult::saved;
}

/**
 * @brief Replaces settings with defaults and optionally preserves Wi-Fi.
 */
SettingsUpdateResult Settings::reset_to_defaults(const bool preserve_wifi, const bool erase_first) {
    auto& candidate = transaction_candidate;
    initialize_default_data(candidate);
    if (preserve_wifi) {
        candidate.ssids = data_.ssids;
        candidate.ssid_lengths = data_.ssid_lengths;
        candidate.passwords = data_.passwords;
        candidate.password_lengths = data_.password_lengths;
        candidate.preferred_wifi_slot = data_.preferred_wifi_slot;
    }

    if (platform::critical_settings_store_enabled()) {
        if (preserve_wifi) {
            if (!save_critical_wifi(candidate, true)) {
                (void)load();
                return SettingsUpdateResult::failed;
            }
        } else {
            const auto removed = platform::remove_critical_settings();
            if (removed != platform::CriticalSettingsResult::succeeded &&
                removed != platform::CriticalSettingsResult::not_found) {
                (void)load();
                return SettingsUpdateResult::failed;
            }
            critical_generation_ = 0U;
            critical_read_only_ = false;
        }
    }

    if (erase_first) {
        // The older slot goes first, so a power cut in between keeps the newest record.
        if (kSlots.remove_both(active_slot_) != filesystem::MutationResult::succeeded) {
            (void)load();
            return SettingsUpdateResult::failed;
        }
        active_slot_ = kNoRecordSlot;
        generation_ = 0;
    }

    if (!save(candidate, true)) {
        if (erase_first) {
            data_ = candidate;
            generation_ = 0;
        }
        return SettingsUpdateResult::failed;
    }
    data_ = candidate;
    save_data_counter_ = data_.power.save_data_seconds;

    // Keep only the reset record. The slot holding the replaced settings goes first, so a
    // failure while removing the sensor policy cannot leave it behind.
    if (kSlots.remove(RecordSlots::target(active_slot_)) != filesystem::MutationResult::succeeded)
        return SettingsUpdateResult::failed;
#if USE_SENSOR_SETTINGS
    if (kSensorSlots.remove_both() != filesystem::MutationResult::succeeded)
        return SettingsUpdateResult::failed;
#endif
    suppress_power_save_until_restart_ = true;
    return SettingsUpdateResult::saved;
}

/**
 * @brief Writes a candidate settings record into the slot not holding the loaded generation.
 *
 * That slot holds the older generation, or is absent or invalid. LittleFS commits the whole
 * file on sync and validates every program, so the loaded record stays intact until the new
 * one is complete, and neither a temporary file nor a read-back is needed.
 * @return true when the candidate became active.
 */
bool Settings::save(const Data& candidate, const bool explicit_reset) {
    if (read_only() && !explicit_reset)
        return false;
    if (filesystem::ensure_directory(kSettingsDirectory) != filesystem::MutationResult::succeeded) {
        return false;
    }

    const std::uint32_t generation = next_generation(generation_);
    auto& encoded = record_scratch;
    const auto encoded_size =
        encode(candidate, generation, !platform::critical_settings_store_enabled(), encoded);
    const auto expected_size =
        (platform::critical_settings_store_enabled() ? kEncodedSizeWithoutWifi : kEncodedSize) -
        (kLightSize - light_encoded_size(candidate.light))
#if !USE_TIMERS
        - (candidate.timer_record_present ? 0U : 4U + kTimersSize)
#endif
        ;
    if (encoded_size != expected_size) {
        return false;
    }

    if (kSlots.write(active_slot_, encoded.data(), encoded_size) !=
        filesystem::MutationResult::succeeded) {
        // The target holds its old content or the complete candidate; the loaded slot is
        // untouched either way, and the next save rewrites the same target.
        return false;
    }
    active_slot_ = RecordSlots::target(active_slot_);
    generation_ = generation;
    read_only_ = false;
    light_dirty_ = false;
    return true;
}

/** @brief Replaces and reads back the authoritative critical Wi-Fi record. */
bool Settings::save_critical_wifi(const Data& candidate, const bool explicit_reset) {
    if (!platform::critical_settings_store_enabled() || (critical_read_only_ && !explicit_reset)) {
        return false;
    }
    const std::uint32_t generation = next_generation(critical_generation_);
    auto& encoded = critical_encoded_scratch;
    encode_critical_wifi(candidate, generation, encoded);
    if (platform::replace_critical_settings(encoded.data(), encoded.size()) !=
            platform::CriticalSettingsResult::succeeded ||
        !verify_critical_encoded(encoded)) {
        return false;
    }
    critical_generation_ = generation;
    critical_read_only_ = false;
    return true;
}

} // namespace newmota

namespace newmota {
SettingsUpdateResult Settings::set_light_settings(
    const LightSettings& value, const bool immediate) {
    if (!value.valid())
        return SettingsUpdateResult::invalid;
    if (read_only())
        return SettingsUpdateResult::failed;
    const auto& old = data_.light;
    if (old.channel_frequency == value.channel_frequency && old.palette == value.palette &&
        old.palette_count == value.palette_count && old.palette_index == value.palette_index &&
        old.fixed_color_index == value.fixed_color_index && old.scheme == value.scheme &&
        old.wakeup_duration == value.wakeup_duration && old.white_dimmer == value.white_dimmer &&
#if USE_WS2812
        old.strip_pixels == value.strip_pixels && old.strip_rotation == value.strip_rotation &&
        old.strip_step_pixels == value.strip_step_pixels && old.strip_width == value.strip_width &&
        old.strip_height == value.strip_height && old.strip_type == value.strip_type &&
        old.strip_reverse == value.strip_reverse && old.strip_alternate == value.strip_alternate &&
        old.strip_clock_colors == value.strip_clock_colors &&
#else
        old.strip_extension_raw == value.strip_extension_raw &&
#endif
        old.strip_extension_present == value.strip_extension_present &&
        old.frequency == value.frequency && old.range == value.range && old.raw == value.raw &&
        old.channels == value.channels && old.rgbww_table == value.rgbww_table &&
        old.led_current == value.led_current && old.color_temperature == value.color_temperature &&
        old.dimmer == value.dimmer && old.speed == value.speed &&
        old.desired_power == value.desired_power && old.gamma == value.gamma &&
        old.fade == value.fade)
        return SettingsUpdateResult::unchanged;
    if (!immediate) {
        data_.light = value;
        light_dirty_ = true;
        return SettingsUpdateResult::pending;
    }
    auto& candidate = begin_transaction(data_);
    candidate.light = value;
    if (!save(candidate))
        return SettingsUpdateResult::failed;
    data_ = candidate;
    return SettingsUpdateResult::saved;
}
} // namespace newmota
