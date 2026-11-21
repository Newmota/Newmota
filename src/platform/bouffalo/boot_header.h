/*
  boot_header.h - target-native Bouffalo boot header geometry

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file boot_header.h
 * @brief Uses the pinned boot2 structs for Wi-Fi6 image header fields.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#if defined(BL616) || defined(BL618DG)
extern "C" {
#include "bflb_port_boot2.h"
}
#endif
namespace newmota::platform::boot_header {
#if defined(BL602)
inline constexpr std::size_t size = 176U;
inline constexpr std::size_t config_offset = 108U;
inline constexpr std::size_t length_offset = 120U;
inline constexpr std::size_t hash_offset = 132U;
inline constexpr std::size_t crc_offset = 172U;
#elif defined(BL616) || defined(BL618DG)
inline constexpr std::size_t size = sizeof(hal_bootheader_t);
inline constexpr std::size_t config_offset = offsetof(hal_bootheader_t, basic_cfg);
inline constexpr std::size_t length_offset = config_offset + offsetof(hal_basic_cfg_t, img_len_cnt);
inline constexpr std::size_t hash_offset = config_offset + offsetof(hal_basic_cfg_t, hash);
inline constexpr std::size_t crc_offset = offsetof(hal_bootheader_t, crc32);
#else
#error "Bouffalo image header has not been reviewed for this target"
#endif
inline constexpr std::uint32_t magic = 0x504E4642U;
static_assert(crc_offset + sizeof(std::uint32_t) == size);
} // namespace newmota::platform::boot_header
