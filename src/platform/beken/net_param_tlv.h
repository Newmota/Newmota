/*
  net_param_tlv.h - bounded Beken NET parameter TLV transformations

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

constexpr std::uint32_t kNetParamTableType = 0x00564c54U;
constexpr std::uint32_t kNewmotaCriticalRecordType = 0x4e4d4352U;

enum class NetParamResult : std::uint8_t {
    succeeded,
    unchanged,
    not_found,
    invalid,
    no_space,
};

/** @brief Extracts one uniquely owned payload from a complete NET sector. */
[[nodiscard]] NetParamResult read_net_param_record(const std::uint8_t* sector,
    std::size_t sector_size, std::uint32_t record_type, void* data, std::size_t capacity,
    std::size_t& size);

/** @brief Rebuilds a NET sector with one record replaced or appended. */
[[nodiscard]] NetParamResult replace_net_param_record(const std::uint8_t* source,
    std::uint8_t* destination, std::size_t sector_size, std::uint32_t record_type, const void* data,
    std::size_t size);

/** @brief Rebuilds a NET sector without one uniquely owned record. */
[[nodiscard]] NetParamResult remove_net_param_record(const std::uint8_t* source,
    std::uint8_t* destination, std::size_t sector_size, std::uint32_t record_type);

} // namespace newmota::platform::beken
