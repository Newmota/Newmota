/*
  critical_settings.cpp - host tests for the common critical-settings route

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "core/storage/filesystem.h"
#include "core/storage/record_slots.h"
#include "newmota/log.h"
#include "newmota/platform/critical_settings.h"
#include "services/settings/settings.h"
#include "support/crc32.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace {

constexpr std::size_t kFileCapacity = 4096U;
constexpr std::size_t kHeaderSize = 20U;
constexpr std::uint16_t kWifiTag = 2U;
constexpr std::uint16_t kPowerTag = 5U;
constexpr std::uint16_t kNtpServersTag = 21U;
constexpr std::uint16_t kButtonDebounceTag = 24U;
constexpr std::uint16_t kButtonMqttTag = 25U;
constexpr std::uint16_t kSwitchMqttTag = 29U;
/** LED PWM fields (3) follow LedState (1) and LedMask (2); older files omit them. */
constexpr std::uint16_t kLedPwmFieldsSize = 3U;

struct FileSlot final {
    std::array<std::uint8_t, kFileCapacity> bytes{};
    std::size_t size{};
    bool present{};
};

std::array<FileSlot, 2> files{};
bool filesystem_available = true;
std::array<std::uint8_t, newmota::platform::kCriticalSettingsCapacity> critical_record{};
std::size_t critical_size = 0U;
bool critical_present = false;
bool critical_replace_failure = false;
bool critical_read_mismatch = false;

[[nodiscard]] FileSlot* file_for(const std::string_view path) {
    if (path == "/.newmota/settings.a")
        return &files[0];
    if (path == "/.newmota/settings.b")
        return &files[1];
    return nullptr;
}

void clear_state() {
    files = {};
    filesystem_available = true;
    critical_record = {};
    critical_size = 0U;
    critical_present = false;
    critical_replace_failure = false;
    critical_read_mismatch = false;
}

[[nodiscard]] std::uint16_t read_u16(const std::uint8_t* const source) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(source[0]) | static_cast<std::uint16_t>(source[1]) << 8U);
}

[[nodiscard]] bool has_tag(const FileSlot& file, const std::uint16_t wanted) {
    std::size_t position = kHeaderSize;
    while (position + 4U <= file.size) {
        const auto tag = read_u16(file.bytes.data() + position);
        const auto size = read_u16(file.bytes.data() + position + 2U);
        position += 4U;
        if (size > file.size - position)
            return false;
        if (tag == wanted)
            return true;
        position += size;
    }
    return false;
}

void write_u16(std::uint8_t* const target, const std::uint16_t value) {
    target[0] = static_cast<std::uint8_t>(value);
    target[1] = static_cast<std::uint8_t>(value >> 8U);
}

/** @brief Returns the generation stored in a slot's header, or 0 for an empty slot. */
[[nodiscard]] std::uint32_t generation_of(const FileSlot& file) {
    if (!file.present || file.size < kHeaderSize)
        return 0U;
    std::uint32_t value = 0U;
    for (unsigned byte = 0U; byte < 4U; ++byte)
        value |= static_cast<std::uint32_t>(file.bytes[8U + byte]) << (8U * byte);
    return value;
}

/** @brief Returns the other settings slot. */
[[nodiscard]] FileSlot& other_slot(const FileSlot& file) {
    return &file == &files[0] ? files[1] : files[0];
}

/** @brief How often only_newest() found the newest record in slot A and in slot B. */
std::array<unsigned, 2> newest_edits{};

/**
 * @brief Returns the slot holding the newest saved record and empties the other one, so a
 * fixture edits the only record in its real slot.
 *
 * Generations compare in the device's serial order and equal ones pick slot A, as load() does.
 */
FileSlot& only_newest() {
    const std::size_t newest =
        !files[0].present || (files[1].present && newmota::newer_generation(generation_of(files[1]),
                                                      generation_of(files[0])))
            ? 1U
            : 0U;
    other_slot(files[newest]) = {};
    ++newest_edits[newest];
    return files[newest];
}

/** @brief Seals a deliberately edited settings fixture as a structurally intact record. */
void seal_file(FileSlot& file) {
    write_u16(file.bytes.data() + 12U, static_cast<std::uint16_t>(file.size - kHeaderSize));
    auto crc = newmota::support::crc32_update(
        newmota::support::kCrc32InitialState, file.bytes.data(), 16U);
    crc = newmota::support::crc32_update(
        crc, file.bytes.data() + kHeaderSize, file.size - kHeaderSize);
    crc = newmota::support::crc32_finalize(crc);
    for (unsigned byte = 0U; byte < 4U; ++byte)
        file.bytes[16U + byte] = static_cast<std::uint8_t>(crc >> (8U * byte));
}

/** @brief Finds a complete TLV header in a saved valid fixture. */
[[nodiscard]] std::size_t record_position(const FileSlot& file, const std::uint16_t tag) {
    for (std::size_t position = kHeaderSize; position + 4U <= file.size;) {
        if (read_u16(file.bytes.data() + position) == tag)
            return position;
        position += 4U + read_u16(file.bytes.data() + position + 2U);
    }
    return file.size;
}

/**
 * @brief Shortens the saved power record by its last bytes, giving it a size this image
 * does not write.
 * @return true when the power record was found and shortened.
 */
bool drop_power_tail(FileSlot& file, const std::uint16_t dropped) {
    std::size_t position = kHeaderSize;
    while (position + 4U <= file.size) {
        const auto tag = read_u16(file.bytes.data() + position);
        const auto size = read_u16(file.bytes.data() + position + 2U);
        if (tag == kPowerTag) {
            const auto end = position + 4U + size;
            const auto cut = end - dropped;
            std::memmove(file.bytes.data() + cut, file.bytes.data() + end, file.size - end);
            file.size -= dropped;
            write_u16(
                file.bytes.data() + position + 2U, static_cast<std::uint16_t>(size - dropped));
            write_u16(file.bytes.data() + 12U, static_cast<std::uint16_t>(file.size - kHeaderSize));
            auto crc = newmota::support::crc32_update(
                newmota::support::kCrc32InitialState, file.bytes.data(), 16U);
            crc = newmota::support::crc32_update(
                crc, file.bytes.data() + kHeaderSize, file.size - kHeaderSize);
            crc = newmota::support::crc32_finalize(crc);
            for (unsigned byte = 0U; byte < 4U; ++byte)
                file.bytes[16U + byte] = static_cast<std::uint8_t>(crc >> (8U * byte));
            other_slot(file) = {};
            return true;
        }
        position += 4U + size;
    }
    return false;
}

void expect(const bool condition, const char* const message, int& failures) {
    if (!condition) {
        std::fprintf(stderr, "critical-settings: %s\n", message);
        ++failures;
    }
}

} // namespace

namespace newmota {
/** @brief Discards application diagnostics in the persistence safety harness. */
void AddLog(LogLevel, const char*, ...) {}
} // namespace newmota

namespace newmota::filesystem {

ReadResult read_file(
    const std::string_view path, void* const data, const std::size_t capacity, std::size_t& size) {
    size = 0U;
    if (!filesystem_available)
        return ReadResult::unavailable;
    const auto* const file = file_for(path);
    if (file == nullptr)
        return ReadResult::invalid_path;
    if (!file->present)
        return ReadResult::not_found;
    if (file->size > capacity)
        return ReadResult::too_large;
    std::memcpy(data, file->bytes.data(), file->size);
    size = file->size;
    return ReadResult::succeeded;
}

MutationResult write_file(
    const std::string_view path, const void* const data, const std::size_t size) {
    if (!filesystem_available)
        return MutationResult::unavailable;
    auto* const file = file_for(path);
    if (file == nullptr || size > file->bytes.size()) {
        return MutationResult::invalid_path;
    }
    std::memcpy(file->bytes.data(), data, size);
    file->size = size;
    file->present = true;
    return MutationResult::succeeded;
}

MutationResult ensure_directory(const std::string_view path) {
    if (!filesystem_available)
        return MutationResult::unavailable;
    return path == "/.newmota" ? MutationResult::succeeded : MutationResult::invalid_path;
}

MutationResult remove_if_exists(const std::string_view path) {
    if (!filesystem_available)
        return MutationResult::unavailable;
    auto* const file = file_for(path);
    if (file == nullptr)
        return MutationResult::invalid_path;
    *file = {};
    return MutationResult::succeeded;
}

} // namespace newmota::filesystem

namespace newmota::platform {

CriticalSettingsResult read_critical_settings(
    void* const data, const std::size_t capacity, std::size_t& size) {
    size = 0U;
    if (!critical_present)
        return CriticalSettingsResult::not_found;
    if (data == nullptr || critical_size > capacity) {
        return CriticalSettingsResult::failed;
    }
    std::memcpy(data, critical_record.data(), critical_size);
    if (critical_read_mismatch && critical_size != 0U) {
        static_cast<std::uint8_t*>(data)[critical_size - 1U] ^= 0x01U;
    }
    size = critical_size;
    return CriticalSettingsResult::succeeded;
}

CriticalSettingsResult replace_critical_settings(const void* const data, const std::size_t size) {
    if (critical_replace_failure || data == nullptr || size == 0U ||
        size > critical_record.size()) {
        return CriticalSettingsResult::failed;
    }
    std::memcpy(critical_record.data(), data, size);
    critical_size = size;
    critical_present = true;
    return CriticalSettingsResult::succeeded;
}

CriticalSettingsResult remove_critical_settings() {
    if (!critical_present)
        return CriticalSettingsResult::not_found;
    critical_record = {};
    critical_size = 0U;
    critical_present = false;
    return CriticalSettingsResult::succeeded;
}

} // namespace newmota::platform

int main() {
    using namespace newmota;
    int failures = 0;

    clear_state();
    Settings settings;
    expect(settings.load() == SettingsLoadResult::defaults, "empty stores use defaults", failures);
    expect(settings.set_ssid(0U, "primary") == SettingsUpdateResult::saved &&
               settings.set_password(0U, "primary-secret") == SettingsUpdateResult::saved &&
               settings.set_ssid(1U, "fallback") == SettingsUpdateResult::saved &&
               settings.set_password(1U, "fallback-secret") == SettingsUpdateResult::saved,
        "both Wi-Fi slots commit through the critical store", failures);
    expect(settings.set_preferred_wifi_slot(1U) == SettingsUpdateResult::saved &&
               settings.preferred_wifi_slot() == 1U,
        "preferred Wi-Fi slot commits through the critical store", failures);
    expect(critical_present && critical_size != 0U, "critical record exists", failures);
    expect(settings.set_option(1U, 1U) == SettingsUpdateResult::saved &&
               !has_tag(only_newest(), kWifiTag),
        "ordinary file excludes critical Wi-Fi fields", failures);

    filesystem_available = false;
    Settings after_update;
    expect(
        after_update.load() == SettingsLoadResult::unavailable &&
            after_update.ssid(0U) == "primary" && after_update.password(0U) == "primary-secret" &&
            after_update.ssid(1U) == "fallback" && after_update.password(1U) == "fallback-secret" &&
            after_update.preferred_wifi_slot() == 1U,
        "critical credentials load while LittleFS is unavailable", failures);

    critical_replace_failure = true;
    expect(after_update.set_ssid(0U, "rejected") == SettingsUpdateResult::failed &&
               after_update.ssid(0U) == "primary",
        "failed replacement leaves live credentials unchanged", failures);
    critical_replace_failure = false;
    critical_read_mismatch = true;
    expect(after_update.set_ssid(0U, "unverified") == SettingsUpdateResult::failed &&
               after_update.ssid(0U) == "primary",
        "failed readback leaves live credentials unchanged", failures);

    clear_state();
    Settings corrupt_source;
    (void)corrupt_source.load();
    expect(corrupt_source.set_ssid(0U, "known") == SettingsUpdateResult::saved,
        "corruption fixture is written", failures);
    critical_record[critical_size - 1U] ^= 0x80U;
    Settings corrupt;
    expect(corrupt.load() == SettingsLoadResult::corrupt && corrupt.ssid(0U).empty() &&
               corrupt.read_only(),
        "corrupt critical record fails closed", failures);

    clear_state();
    Settings reset;
    (void)reset.load();
    (void)reset.set_ssid(0U, "keep-me");
    (void)reset.set_password(0U, "keep-secret");
    (void)reset.set_preferred_wifi_slot(1U);
    (void)reset.set_option(1U, 1U);
    expect(reset.reset_to_defaults(true, false) == SettingsUpdateResult::saved &&
               reset.ssid(0U) == "keep-me" && reset.preferred_wifi_slot() == 1U && critical_present,
        "preserving reset rewrites critical Wi-Fi settings", failures);
    expect(reset.reset_to_defaults(false, false) == SettingsUpdateResult::saved &&
               reset.ssid(0U).empty() && reset.preferred_wifi_slot() == 0U && !critical_present,
        "discarding reset removes only the owned critical record", failures);

    clear_state();
    Settings ntp;
    (void)ntp.load();
    expect(ntp.set_ntp_server(0U, "time.example") == SettingsUpdateResult::saved &&
               ntp.set_ntp_server(1U, "192.0.2.123") == SettingsUpdateResult::saved &&
               ntp.set_ntp_server(2U, "") == SettingsUpdateResult::saved,
        "NTP slots commit independently with an intentionally empty slot", failures);
    const auto ntp_generation = ntp.generation();
    expect(ntp.set_ntp_server(0U, "time.example") == SettingsUpdateResult::unchanged &&
               ntp.generation() == ntp_generation &&
               ntp.set_ntp_server(0U, "http://time.example") == SettingsUpdateResult::invalid &&
               ntp.set_ntp_server(0U, "time.example.") == SettingsUpdateResult::invalid &&
               ntp.set_ntp_server(1U, "192.0.2.999") == SettingsUpdateResult::invalid &&
               ntp.set_ntp_server(3U, "time.example") == SettingsUpdateResult::invalid,
        "unchanged and invalid NTP addresses cannot rewrite settings", failures);
    filesystem_available = false;
    expect(ntp.set_ntp_server(0U, "rejected.example") == SettingsUpdateResult::failed &&
               ntp.ntp_server(0U) == "time.example" && ntp.generation() == ntp_generation,
        "failed NTP persistence preserves live state and generation", failures);
    filesystem_available = true;
    Settings ntp_reloaded;
    expect(ntp_reloaded.load() == SettingsLoadResult::loaded &&
               ntp_reloaded.ntp_server(0U) == "time.example" &&
               ntp_reloaded.ntp_server(1U) == "192.0.2.123" &&
               ntp_reloaded.ntp_server(2U).empty() &&
               ntp_reloaded.set_time_format(2U) == SettingsUpdateResult::saved,
        "profiles without Time preserve NTP slots through a reload and unrelated save", failures);
    Settings ntp_preserved;
    expect(ntp_preserved.load() == SettingsLoadResult::loaded &&
               ntp_preserved.ntp_server(0U) == "time.example" &&
               ntp_preserved.ntp_server(2U).empty(),
        "unrelated writes retain the NTP record without the Time feature", failures);
    std::array<char, Settings::kNtpServerCapacity + 1U> maximum_address{};
    maximum_address.fill('a');
    expect(
        ntp_preserved.set_ntp_server(0U, {maximum_address.data(), Settings::kNtpServerCapacity}) ==
                SettingsUpdateResult::saved &&
            ntp_preserved.set_ntp_server(0U, {maximum_address.data(), maximum_address.size()}) ==
                SettingsUpdateResult::invalid,
        "NTP address capacity is enforced before persistence", failures);
    Settings ntp_maximum;
    expect(ntp_maximum.load() == SettingsLoadResult::loaded &&
               ntp_maximum.ntp_server(0U).size() == Settings::kNtpServerCapacity,
        "a maximum-length NTP address reloads without a terminator", failures);
    auto& ntp_file = only_newest();
    const FileSlot saved_ntp_file = ntp_file;
    const auto ntp_position = record_position(saved_ntp_file, kNtpServersTag);
    expect(ntp_position < saved_ntp_file.size, "NTP settings have a separate tag", failures);
    if (ntp_position < saved_ntp_file.size) {
        const auto ntp_size = 4U + read_u16(saved_ntp_file.bytes.data() + ntp_position + 2U);
        std::memmove(ntp_file.bytes.data() + ntp_position,
            ntp_file.bytes.data() + ntp_position + ntp_size,
            ntp_file.size - ntp_position - ntp_size);
        ntp_file.size -= ntp_size;
        seal_file(ntp_file);
        other_slot(ntp_file) = {};
        Settings before_ntp;
        expect(before_ntp.load() == SettingsLoadResult::loaded && !before_ntp.read_only() &&
                   before_ntp.time_format() == 2U &&
                   before_ntp.ntp_server(0U) == Settings::default_ntp_server(0U) &&
                   before_ntp.ntp_server(1U) == Settings::default_ntp_server(1U) &&
                   before_ntp.ntp_server(2U) == Settings::default_ntp_server(2U),
            "older settings without NTP tag retain time fields and use per-slot defaults",
            failures);
        for (unsigned corruption = 0; corruption < 4; ++corruption) {
            ntp_file = saved_ntp_file;
            other_slot(ntp_file) = {};
            if (corruption == 0) {
                ntp_file.bytes[ntp_position + 4U] =
                    static_cast<std::uint8_t>(Settings::kNtpServerCapacity + 1U);
            } else if (corruption == 1) {
                ntp_file.bytes[ntp_position + 5U] = 0U;
            } else if (corruption == 2) {
                std::memcpy(ntp_file.bytes.data() + ntp_file.size,
                    saved_ntp_file.bytes.data() + ntp_position, ntp_size);
                ntp_file.size += ntp_size;
            } else {
                write_u16(ntp_file.bytes.data() + ntp_position + 2U,
                    static_cast<std::uint16_t>(ntp_size - 5U));
            }
            seal_file(ntp_file);
            Settings malformed_ntp;
            expect(malformed_ntp.load() == SettingsLoadResult::corrupt &&
                       malformed_ntp.read_only() &&
                       malformed_ntp.set_time_format(3U) == SettingsUpdateResult::failed,
                "malformed NTP TLV locks writes before unrelated settings can erase it", failures);
        }
    }

    clear_state();
    Settings buttons;
    expect(buttons.load() == SettingsLoadResult::defaults && buttons.button_debounce() == 50U &&
               buttons.set_button_debounce(53U) == SettingsUpdateResult::saved &&
               buttons.set_time_format(2U) == SettingsUpdateResult::saved,
        "ButtonDebounce defaults to 50 and saves flags alongside unrelated settings", failures);
    const auto button_generation = buttons.generation();
    expect(buttons.set_button_debounce(53U) == SettingsUpdateResult::unchanged &&
               buttons.set_button_debounce(39U) == SettingsUpdateResult::invalid &&
               buttons.set_button_debounce(1001U) == SettingsUpdateResult::invalid &&
               buttons.set_button_debounce(49U) == SettingsUpdateResult::invalid &&
               buttons.generation() == button_generation && buttons.button_debounce() == 53U,
        "unchanged, out-of-range and unsupported AC values cannot rewrite settings", failures);
    Settings buttons_reloaded;
    expect(buttons_reloaded.load() == SettingsLoadResult::loaded &&
               buttons_reloaded.button_debounce() == 53U && buttons_reloaded.time_format() == 2U,
        "ButtonDebounce survives reload without changing unrelated fields", failures);
    auto& button_file = only_newest();
    const FileSlot saved_button_file = button_file;
    const auto button_position = record_position(saved_button_file, kButtonDebounceTag);
    expect(button_position < saved_button_file.size, "ButtonDebounce has a separate tag", failures);
    if (button_position < saved_button_file.size) {
        constexpr std::size_t kButtonRecordSize = 6U;
        std::memmove(button_file.bytes.data() + button_position,
            button_file.bytes.data() + button_position + kButtonRecordSize,
            button_file.size - button_position - kButtonRecordSize);
        button_file.size -= kButtonRecordSize;
        other_slot(button_file) = {};
        seal_file(button_file);
        Settings before_buttons;
        expect(before_buttons.load() == SettingsLoadResult::loaded && !before_buttons.read_only() &&
                   before_buttons.button_debounce() == 50U && before_buttons.time_format() == 2U,
            "older files without ButtonDebounce retain unrelated settings and use default 50",
            failures);
        for (unsigned corruption = 0U; corruption < 3U; ++corruption) {
            button_file = saved_button_file;
            other_slot(button_file) = {};
            if (corruption == 0U) {
                write_u16(button_file.bytes.data() + button_position + 4U, 49U);
            } else if (corruption == 1U) {
                write_u16(button_file.bytes.data() + button_position + 2U, 1U);
            } else {
                std::memcpy(button_file.bytes.data() + button_file.size,
                    saved_button_file.bytes.data() + button_position, kButtonRecordSize);
                button_file.size += kButtonRecordSize;
            }
            seal_file(button_file);
            Settings malformed_buttons;
            expect(malformed_buttons.load() == SettingsLoadResult::corrupt &&
                       malformed_buttons.read_only() &&
                       malformed_buttons.set_time_format(3U) == SettingsUpdateResult::failed,
                "invalid, short or duplicate ButtonDebounce records lock unrelated writes",
                failures);
        }
    }

    clear_state();
    Settings switches;
    expect(switches.load() == SettingsLoadResult::defaults && switches.switch_debounce() == 50U &&
               switches.switch_modes()[0] == 0U &&
               switches.set_switch_debounce(52U) == SettingsUpdateResult::saved &&
               switches.set_switch_mode(0U, 1U) == SettingsUpdateResult::saved &&
               switches.set_switch_mode(32U, 16U) == SettingsUpdateResult::saved,
        "SwitchDebounce and SwitchMode default like Tasmota and save", failures);
    const auto switch_generation = switches.generation();
    expect(switches.set_switch_mode(33U, 1U) == SettingsUpdateResult::invalid &&
               switches.set_switch_mode(1U, 17U) == SettingsUpdateResult::invalid &&
               switches.set_switch_debounce(59U) == SettingsUpdateResult::invalid &&
               switches.set_switch_mode(2U, 1U) == SettingsUpdateResult::unchanged &&
               switches.generation() == switch_generation,
        "invalid or unchanged switch settings cannot rewrite settings", failures);
    Settings switches_reloaded;
    expect(switches_reloaded.load() == SettingsLoadResult::loaded &&
               switches_reloaded.switch_debounce() == 52U &&
               switches_reloaded.switch_modes()[0] == 1U &&
               switches_reloaded.switch_modes()[30] == 1U &&
               switches_reloaded.switch_modes()[31] == 16U,
        "switch settings survive reload", failures);
    expect(switches_reloaded.set_switch_text(1U, "Door") == SettingsUpdateResult::saved &&
               switches_reloaded.set_switch_text(28U, "abcdefghijklmno") ==
                   SettingsUpdateResult::saved &&
               switches_reloaded.set_switch_text(29U, "x") == SettingsUpdateResult::invalid &&
               switches_reloaded.set_switch_text(2U, "a b") == SettingsUpdateResult::invalid &&
               switches_reloaded.set_switch_text(2U, "abcdefghijklmnop") ==
                   SettingsUpdateResult::invalid &&
               switches_reloaded.set_switch_text(2U, "a\"b") == SettingsUpdateResult::invalid &&
               switches_reloaded.set_switch_text(2U, "a\\b") == SettingsUpdateResult::invalid,
        "SwitchText saves bounded printable text without spaces, quotes or backslashes", failures);
    auto& switch_file = only_newest();
    const auto switch_mqtt_position = record_position(switch_file, kSwitchMqttTag);
    expect(switch_mqtt_position < switch_file.size, "switch MQTT has a separate tag", failures);
    if (switch_mqtt_position < switch_file.size) {
        constexpr std::string_view kRemote = "walls";
        switch_file.bytes[switch_mqtt_position + 4U] = static_cast<std::uint8_t>(kRemote.size());
        switch_file.bytes[switch_mqtt_position + 5U] = 1U;
        std::memcpy(
            switch_file.bytes.data() + switch_mqtt_position + 6U, kRemote.data(), kRemote.size());
        seal_file(switch_file);
        other_slot(switch_file) = {};
    }
    Settings switch_texts;
    expect(switch_texts.load() == SettingsLoadResult::loaded &&
               switch_texts.switch_text(1U) == "Door" &&
               switch_texts.switch_text(28U) == "abcdefghijklmno" &&
               switch_texts.switch_text(2U).empty() &&
               switch_texts.mqtt_settings().switch_topic() == "walls" &&
               switch_texts.mqtt_settings().switch_retain &&
               switch_texts.mqtt_settings().button_topic().empty(),
        "SwitchText and SwitchTopic/SwitchRetain survive reload", failures);
    auto tuya = switch_texts.tuya_settings();
    tuya.functions[0] = {11U, 1U};
    tuya.functions[15] = {99U, 7U};
    tuya.stat_topic = true;
    tuya.dimmer_min = 25U;
    tuya.dimmer_max = 1000U;
    tuya.rgb_type = 3U;
    tuya.temp_set_res = 2U;
    tuya.enum_max = {3U, 0U, 31U, 1U};
    expect(switch_texts.set_tuya_settings(tuya) == SettingsUpdateResult::saved &&
               switch_texts.set_tuya_settings(tuya) == SettingsUpdateResult::unchanged,
        "TuyaMCU settings save once", failures);
    Settings tuya_reloaded;
    expect(tuya_reloaded.load() == SettingsLoadResult::loaded &&
               tuya_reloaded.tuya_settings() == tuya && tuya_reloaded.switch_text(1U) == "Door",
        "TuyaMCU settings survive reload", failures);

    clear_state();
    Settings button_mqtt_source;
    (void)button_mqtt_source.load();
    expect(button_mqtt_source.set_time_format(2U) == SettingsUpdateResult::saved,
        "button MQTT persistence fixture is written", failures);
    auto& button_mqtt_file = only_newest();
    const auto button_mqtt_position = record_position(button_mqtt_file, kButtonMqttTag);
    expect(button_mqtt_position < button_mqtt_file.size, "button MQTT has a separate retained tag",
        failures);
    if (button_mqtt_position < button_mqtt_file.size) {
        constexpr std::string_view kRemote = "remote_%06X";
        constexpr std::size_t kRecordSize = 4U + 2U + MqttSettings::kTopicCapacity;
        button_mqtt_file.bytes[button_mqtt_position + 4U] =
            static_cast<std::uint8_t>(kRemote.size());
        button_mqtt_file.bytes[button_mqtt_position + 5U] = 1U;
        std::memcpy(button_mqtt_file.bytes.data() + button_mqtt_position + 6U, kRemote.data(),
            kRemote.size());
        seal_file(button_mqtt_file);
        other_slot(button_mqtt_file) = {};
        Settings button_mqtt;
        expect(button_mqtt.load() == SettingsLoadResult::loaded &&
                   button_mqtt.mqtt_settings().button_topic() == kRemote &&
                   button_mqtt.mqtt_settings().button_retain &&
                   button_mqtt.set_time_format(3U) == SettingsUpdateResult::saved,
            "profiles without MQTT runtime preserve ButtonTopic/retain during unrelated saves",
            failures);
        Settings button_mqtt_again;
        expect(button_mqtt_again.load() == SettingsLoadResult::loaded &&
                   button_mqtt_again.mqtt_settings().button_topic() == kRemote &&
                   button_mqtt_again.mqtt_settings().button_retain,
            "retained button MQTT settings survive round-trip", failures);
        // The unrelated save above wrote the other slot; edit that newer record.
        auto& resaved = only_newest();
        const auto saved = resaved;
        std::memmove(resaved.bytes.data() + button_mqtt_position,
            resaved.bytes.data() + button_mqtt_position + kRecordSize,
            resaved.size - button_mqtt_position - kRecordSize);
        resaved.size -= kRecordSize;
        seal_file(resaved);
        other_slot(resaved) = {};
        Settings old_buttons;
        std::uint8_t hold{};
        expect(old_buttons.load() == SettingsLoadResult::loaded && !old_buttons.read_only() &&
                   old_buttons.mqtt_settings().button_topic().empty() &&
                   !old_buttons.mqtt_settings().button_retain && old_buttons.option(32U, hold) &&
                   hold == 40U && old_buttons.time_format() == 3U,
            "a file without the button record defaults button routing off", failures);
        for (unsigned corruption = 0U; corruption < 4U; ++corruption) {
            resaved = saved;
            other_slot(resaved) = {};
            if (corruption == 0U)
                resaved.bytes[button_mqtt_position + 5U] = 2U;
            else if (corruption == 1U)
                write_u16(resaved.bytes.data() + button_mqtt_position + 2U, 1U);
            else if (corruption == 2U) {
                std::memcpy(resaved.bytes.data() + resaved.size,
                    saved.bytes.data() + button_mqtt_position, kRecordSize);
                resaved.size += kRecordSize;
            } else
                write_u16(resaved.bytes.data() + button_mqtt_position, 0x7FFFU);
            seal_file(resaved);
            Settings invalid_button_mqtt;
            const auto loaded = invalid_button_mqtt.load();
            expect((corruption == 3U ? loaded == SettingsLoadResult::loaded
                                     : loaded == SettingsLoadResult::corrupt) &&
                       invalid_button_mqtt.read_only() &&
                       invalid_button_mqtt.set_time_format(1U) == SettingsUpdateResult::failed,
                "invalid, short, duplicate or unknown button MQTT records protect unrelated writes",
                failures);
        }
    }

    clear_state();
    Settings leds;
    (void)leds.load();
    expect(leds.power_settings().led_state == 1U && leds.power_settings().led_mask == 0xFFFFU,
        "LedState and LedMask default to Tasmota values", failures);
    expect(leds.set_blink_time(25U) == SettingsUpdateResult::saved &&
               leds.set_led_state(3U) == SettingsUpdateResult::saved &&
               leds.set_led_mask(0x0005U) == SettingsUpdateResult::saved &&
               leds.set_led_pwm(0x03U, 200U, 10U) == SettingsUpdateResult::saved,
        "LED policy and a relay setting are saved", failures);
    Settings leds_reloaded;
    expect(leds_reloaded.load() == SettingsLoadResult::loaded &&
               leds_reloaded.power_settings().led_state == 3U &&
               leds_reloaded.power_settings().led_mask == 0x0005U &&
               leds_reloaded.power_settings().led_pwm_mask == 0x03U &&
               leds_reloaded.power_settings().led_pwm_on == 200U &&
               leds_reloaded.power_settings().led_pwm_off == 10U &&
               leds_reloaded.power_settings().blink_time == 25U,
        "LED policy survives a reload", failures);
    expect(
        drop_power_tail(only_newest(), kLedPwmFieldsSize), "power record is shortened", failures);
    Settings other_size;
    expect(other_size.load() == SettingsLoadResult::loaded && !other_size.read_only() &&
               other_size.power_settings().blink_time != 25U &&
               other_size.power_settings().led_state == 1U &&
               other_size.power_settings().led_mask == 0xFFFFU &&
               other_size.power_settings().led_pwm_on == 255U,
        "a power record of another size reads as missing and the file stays writable", failures);
    expect(other_size.set_led_state(9U) == SettingsUpdateResult::invalid &&
               other_size.set_led_pwm(0x10U, 255U, 0U) == SettingsUpdateResult::invalid,
        "LedState above 8 and LedPwmMode beyond four LEDs are rejected", failures);

    clear_state();
    Settings slots;
    (void)slots.load();
    expect(slots.set_time_format(1U) == SettingsUpdateResult::saved &&
               generation_of(files[0]) == 1U && !files[1].present &&
               slots.set_time_format(2U) == SettingsUpdateResult::saved &&
               generation_of(files[0]) == 1U && generation_of(files[1]) == 2U &&
               slots.set_time_format(3U) == SettingsUpdateResult::saved &&
               generation_of(files[0]) == 3U && generation_of(files[1]) == 2U,
        "saves alternate between the two slots and overwrite the older one", failures);
    Settings slots_reloaded;
    expect(slots_reloaded.load() == SettingsLoadResult::loaded && !slots_reloaded.read_only() &&
               slots_reloaded.generation() == 3U && slots_reloaded.time_format() == 3U,
        "the newest generation wins whichever slot holds it", failures);

    files[0].bytes[files[0].size - 1U] ^= 0x01U;
    const FileSlot bad_newest = files[0];
    Settings fallback;
    expect(fallback.load() == SettingsLoadResult::recovered && fallback.read_only() &&
               fallback.generation() == 2U && fallback.time_format() == 2U &&
               fallback.set_time_format(1U) == SettingsUpdateResult::failed &&
               std::memcmp(files[0].bytes.data(), bad_newest.bytes.data(), bad_newest.size) == 0 &&
               generation_of(files[1]) == 2U,
        "a bad newer slot falls back to the other one and locks writes", failures);
    expect(fallback.reset_to_defaults(false, false) == SettingsUpdateResult::saved &&
               !fallback.read_only() && generation_of(files[0]) == 3U && !files[1].present,
        "an explicit reset replaces the bad slot and keeps only the reset record", failures);
    expect(fallback.set_time_format(2U) == SettingsUpdateResult::saved &&
               generation_of(files[1]) == 4U &&
               fallback.reset_to_defaults(false, true) == SettingsUpdateResult::saved &&
               generation_of(files[0]) == 1U && !files[1].present,
        "an erasing reset removes both slots and writes slot A", failures);

    clear_state();
    Settings empty_slot;
    (void)empty_slot.load();
    (void)empty_slot.set_time_format(2U);
    files[1].present = true;
    files[1].size = 0U;
    Settings after_empty;
    expect(after_empty.load() == SettingsLoadResult::loaded && !after_empty.read_only() &&
               after_empty.generation() == 1U &&
               after_empty.set_time_format(3U) == SettingsUpdateResult::saved &&
               generation_of(files[1]) == 2U,
        "a 0-byte slot counts as absent and is written next", failures);

    write_u16(files[0].bytes.data() + 8U, 0xFFFFU);
    write_u16(files[0].bytes.data() + 10U, 0xFFFFU);
    seal_file(files[0]);
    files[1] = {};
    Settings wrap;
    expect(wrap.load() == SettingsLoadResult::loaded && wrap.generation() == UINT32_MAX &&
               wrap.set_time_format(1U) == SettingsUpdateResult::saved &&
               generation_of(files[1]) == 1U,
        "the generation after UINT32_MAX is 1", failures);
    Settings wrapped;
    expect(wrapped.load() == SettingsLoadResult::loaded && wrapped.generation() == 1U &&
               wrapped.time_format() == 1U &&
               wrapped.set_time_format(3U) == SettingsUpdateResult::saved &&
               generation_of(files[0]) == 2U,
        "generation 1 is newer than UINT32_MAX and the next save replaces the older slot",
        failures);

    expect(newest_edits[0] != 0U && newest_edits[1] != 0U,
        "fixture edits and reloads ran with the newest record in slot A and in slot B", failures);

    if (failures == 0) {
        std::puts("Critical settings tests passed");
    }
    return failures == 0 ? 0 : 1;
}
