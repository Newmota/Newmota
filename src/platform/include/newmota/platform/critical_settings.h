/*
  critical_settings.h - bounded OTA-critical record transport for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstddef>
#include <cstdint>

#ifndef NEWMOTA_CRITICAL_SETTINGS_STORE
#define NEWMOTA_CRITICAL_SETTINGS_STORE 0
#endif

namespace newmota::platform {

constexpr std::size_t kCriticalSettingsCapacity = 512;

enum class CriticalSettingsResult : std::uint8_t {
    succeeded,
    not_found,
    unavailable,
    failed,
};

#if NEWMOTA_CRITICAL_SETTINGS_STORE

/** @brief Reports that the resolved layout selected a critical-record store. */
[[nodiscard]] constexpr bool critical_settings_store_enabled() {
    return true;
}

/** @brief Reads the one bounded opaque record into caller-owned storage. */
[[nodiscard]] CriticalSettingsResult read_critical_settings(
    void* data, std::size_t capacity, std::size_t& size);
/** @brief Atomically replaces the one bounded opaque record where possible. */
[[nodiscard]] CriticalSettingsResult replace_critical_settings(const void* data, std::size_t size);
/** @brief Removes the owned record while preserving native neighboring data. */
[[nodiscard]] CriticalSettingsResult remove_critical_settings();

#else

/** @brief Reports that the resolved layout needs no second settings store. */
[[nodiscard]] constexpr bool critical_settings_store_enabled() {
    return false;
}

[[nodiscard]] inline CriticalSettingsResult read_critical_settings(
    void*, std::size_t, std::size_t& size) {
    size = 0;
    return CriticalSettingsResult::unavailable;
}

[[nodiscard]] inline CriticalSettingsResult replace_critical_settings(const void*, std::size_t) {
    return CriticalSettingsResult::unavailable;
}

[[nodiscard]] inline CriticalSettingsResult remove_critical_settings() {
    return CriticalSettingsResult::unavailable;
}

#endif

} // namespace newmota::platform
