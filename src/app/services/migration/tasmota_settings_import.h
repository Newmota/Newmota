/*
  tasmota_settings_import.h - ESP32 Tasmota settings bootstrap import

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

namespace newmota {
class Settings;
}

namespace newmota::migration {

enum class TasmotaSettingsImportStatus : unsigned char {
    imported,
    not_found,
    no_wifi,
    unsupported,
    invalid,
    unavailable,
    write_failed,
};

struct TasmotaSettingsImportResult final {
    TasmotaSettingsImportStatus status{};
    const char* source{};
};

/** @brief Imports supported Tasmota Wi-Fi slots without changing source files. */
[[nodiscard]] TasmotaSettingsImportResult import_tasmota_wifi(Settings& settings);

} // namespace newmota::migration
