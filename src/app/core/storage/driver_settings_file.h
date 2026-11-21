/*
  driver_settings_file.h - Per-driver fixed-size settings file

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file driver_settings_file.h
 * @brief A CRC-protected settings file owned by one driver, outside the shared settings record.
 *
 * Safeboot and the application share one tagged settings file, and Safeboot goes read-only on a
 * tag it does not know. A driver that only the application uses therefore keeps its own file:
 * Safeboot never reads it and the shared record does not grow. The driver names its file,
 * supplies defaults in the payload buffer, and gets them back overlaid with the stored values.
 *
 * Layout: magic (4), version (1), payload, CRC-32 of everything before it (4, little endian).
 * A different version is treated like corruption; drivers keep their payload layout stable or
 * bump the version and accept falling back to defaults.
 */
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace newmota {
/** @brief Largest payload a driver file can hold. */
inline constexpr std::size_t kDriverSettingsMaximumPayload = 128;

/** @brief Identity and layout of one driver's settings file. */
struct DriverSettingsSpec final {
    /** @brief Active, temporary and recovery paths, for example `/.newmota/audio.bin`. */
    const char* path;
    const char* temporary;
    const char* previous;
    std::array<std::uint8_t, 4> magic;
    std::uint8_t version;
    std::size_t payload_size;
    /** @brief Semantic check of a CRC-clean payload; null accepts any payload. */
    bool (*valid)(const std::uint8_t* payload);
};

enum class DriverSettingsState : std::uint8_t {
    /** @brief The active file was read. */
    loaded,
    /** @brief The active file was unusable and the previous generation was read. */
    recovered,
    /** @brief No file existed; the defaults were written as the first file. */
    created,
    /** @brief The file was unusable and no recovery existed; defaults replaced it. */
    defaulted,
    /** @brief The filesystem is unavailable; the defaults apply for this boot only. */
    failed,
};

/** @brief Loads the file into @p payload, creating it from the incoming defaults when missing.
 * @param payload `spec.payload_size` bytes holding the defaults on entry; only a valid stored
 *        payload replaces them.
 */
[[nodiscard]] DriverSettingsState load_driver_settings(
    const DriverSettingsSpec& spec, std::uint8_t* payload);

/** @brief Replaces the file using a verified temporary and a recovery generation.
 * @return false when nothing was changed or the previous generation was restored.
 */
[[nodiscard]] bool save_driver_settings(
    const DriverSettingsSpec& spec, const std::uint8_t* payload);
} // namespace newmota
