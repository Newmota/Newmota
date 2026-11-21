/*
  tasmota_settings_import.cpp - ESP32 Tasmota settings bootstrap import

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file tasmota_settings_import.cpp
 * @brief ESP32 Tasmota settings bootstrap import.
 */

#include "services/migration/tasmota_settings_import.h"

#include "core/storage/filesystem.h"
#include "services/migration/tasmota_settings.h"
#include "services/settings/settings.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string_view>

namespace newmota::migration {
namespace {

constexpr char kPrimaryPath[] = "/.settings";
constexpr char kLastKnownGoodPath[] = "/.settings.lkg";
using TasmotaFileBuffer = std::array<std::uint8_t, kTasmotaSettingsSize>;

struct FileResult final {
    TasmotaSettingsImportStatus status{TasmotaSettingsImportStatus::not_found};
    TasmotaCredentials settings{};
    const char* source{};
};

[[nodiscard]] FileResult read_candidate(const char* const path, TasmotaFileBuffer& buffer) {
    FileResult result{};
    result.source = path;
    std::size_t size = 0U;
    const auto read = filesystem::read_file(path, buffer.data(), buffer.size(), size);
    if (read == filesystem::ReadResult::not_found) {
        return result;
    }
    if (read == filesystem::ReadResult::unavailable) {
        result.status = TasmotaSettingsImportStatus::unavailable;
        return result;
    }
    if (read != filesystem::ReadResult::succeeded) {
        result.status = TasmotaSettingsImportStatus::invalid;
        return result;
    }

    switch (parse_tasmota_settings(buffer.data(), size, result.settings)) {
    case TasmotaSettingsParseResult::valid:
        result.status = TasmotaSettingsImportStatus::imported;
        break;
    case TasmotaSettingsParseResult::unsupported:
        result.status = TasmotaSettingsImportStatus::unsupported;
        break;
    case TasmotaSettingsParseResult::invalid:
        result.status = TasmotaSettingsImportStatus::invalid;
        break;
    }
    return result;
}

} // namespace

TasmotaSettingsImportResult import_tasmota_wifi(Settings& settings) {
    // The source record is needed only during first-boot migration; avoid reserving 4 KB
    // of static RAM for the lifetime of every application that includes the importer.
    std::unique_ptr<TasmotaFileBuffer> buffer{new (std::nothrow) TasmotaFileBuffer{}};
    if (!buffer) {
        return {TasmotaSettingsImportStatus::unavailable, nullptr};
    }

    auto candidate = read_candidate(kPrimaryPath, *buffer);
    if (candidate.status != TasmotaSettingsImportStatus::imported) {
        const auto previous = read_candidate(kLastKnownGoodPath, *buffer);
        if (previous.status == TasmotaSettingsImportStatus::imported) {
            candidate = previous;
        } else if (candidate.status == TasmotaSettingsImportStatus::not_found) {
            candidate = previous;
        } else if (previous.status != TasmotaSettingsImportStatus::not_found &&
                   previous.status != TasmotaSettingsImportStatus::invalid &&
                   previous.status != TasmotaSettingsImportStatus::unsupported) {
            candidate = previous;
        }
    }

    if (candidate.status != TasmotaSettingsImportStatus::imported) {
        return {candidate.status, candidate.source};
    }

    bool has_wifi = false;
    std::array<std::string_view, Settings::kWifiSlotCount> ssids{};
    std::array<std::string_view, Settings::kWifiSlotCount> passwords{};
    for (std::size_t slot = 0; slot < Settings::kWifiSlotCount; ++slot) {
        const auto length = candidate.settings.ssid_lengths[slot];
        ssids[slot] = {candidate.settings.ssids[slot].data(), length};
        const auto password_length = candidate.settings.password_lengths[slot];
        passwords[slot] = {candidate.settings.passwords[slot].data(), password_length};
        has_wifi = has_wifi || length != 0U;
    }
    if (!has_wifi) {
        return {TasmotaSettingsImportStatus::no_wifi, candidate.source};
    }

    const auto saved = settings.set_wifi_credentials(ssids, passwords,
        {candidate.settings.web_password.data(), candidate.settings.web_password_length},
        candidate.settings.preferred_slot);
    if (saved != SettingsUpdateResult::saved && saved != SettingsUpdateResult::unchanged) {
        return {TasmotaSettingsImportStatus::write_failed, candidate.source};
    }
    return {TasmotaSettingsImportStatus::imported, candidate.source};
}

} // namespace newmota::migration
