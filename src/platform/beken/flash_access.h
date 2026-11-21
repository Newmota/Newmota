/*
  flash_access.h - checked physical-flash operations for Beken adapters

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace newmota::platform::beken {

constexpr std::size_t kFlashSectorSize = 4096U;

enum class SectorMutationResult : std::uint8_t {
    changed,
    unchanged,
    failed,
};

using SectorMutator = SectorMutationResult (*)(
    void* context, const std::uint8_t* source, std::uint8_t* destination, std::size_t size);

[[nodiscard]] bool read_flash(std::uint32_t address, void* data, std::size_t size);
[[nodiscard]] bool program_flash(std::uint32_t address, const void* data, std::size_t size);
[[nodiscard]] bool erase_flash_sector(std::uint32_t address);
[[nodiscard]] bool mutate_flash_sector(std::uint32_t address, SectorMutator mutator, void* context);

/** @brief Reserve native OTA staging; general writes into it are refused until release. */
[[nodiscard]] bool begin_ota_staging();
/** @brief Release the staging lease. LittleFS lies outside staging and is never blocked. */
[[nodiscard]] bool release_ota_staging();
[[nodiscard]] bool ota_staging_available();
[[nodiscard]] bool read_ota_staging(std::uint32_t address, void* data, std::size_t size);
[[nodiscard]] bool program_ota_staging(std::uint32_t address, const void* data, std::size_t size);
[[nodiscard]] bool erase_ota_staging_sector(std::uint32_t address);

} // namespace newmota::platform::beken
