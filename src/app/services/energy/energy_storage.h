/*
  energy_storage.h - Energy record validation without native resources

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file energy_storage.h
 * @brief Energy record validation without native resources.
 */

#pragma once
#include "services/energy/energy_types.h"

namespace newmota::energy::storage {
/** @brief Record format byte; a file with any other value is not an energy record. */
inline constexpr std::uint8_t kFormat = 27;
/** @brief Header: magic (4), format (1), A/B slot generation (4). */
inline constexpr std::size_t kGenerationOffset = 5, kHeaderSize = 9, kMaximumChannels = 32;

/** @brief Reports whether bytes carry the energy magic, independently of format and checksum. */
bool has_magic(const std::uint8_t* bytes, std::size_t size);
/** @brief Checks the entire CRC-protected record before any field is used. */
bool validate(const std::uint8_t* bytes, std::size_t size);
/** @brief Writes one little-endian field to a pre-sized output buffer. */
void put(std::uint8_t*& cursor, std::uint64_t value, std::size_t bytes);
/** @brief Reads one field after complete-record validation or a caller bounds check. */
std::uint64_t get(const std::uint8_t*& cursor, std::size_t bytes);
/** @brief Computes the record's IEEE CRC32. */
std::uint32_t crc(const std::uint8_t* bytes, std::size_t size);
/** @brief Decodes a bounded physical identity, calibration and operating options. */
bool read_configuration(
    const std::uint8_t*& cursor, const std::uint8_t* end, Configuration& config);
/** @brief Decodes a bounded retained dated account. */
bool read_calendar(const std::uint8_t*& cursor, const std::uint8_t* end, CalendarData& calendar,
    std::uint8_t& flags);
/** @brief Encodes exact dated accounts independently of feature selection. */
void put_calendar(std::uint8_t*& cursor, const CalendarData* calendar, bool fault);
/** @brief Decodes a bounded tariff schedule. */
bool read_schedule(const std::uint8_t*& cursor, const std::uint8_t* end, TariffSchedule& schedule);
/** @brief Decodes exact directional tariff accounts. */
bool read_tariff(
    const std::uint8_t*& cursor, const std::uint8_t* end, TariffData& tariff, std::uint8_t& flags);
/** @brief Decodes exact source-compatible signed tariff accounts. */
bool read_tariff_usage(
    const std::uint8_t*& cursor, const std::uint8_t* end, TariffUsage& values, std::uint8_t& flags);
} // namespace newmota::energy::storage
