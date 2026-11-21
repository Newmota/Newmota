/*
  image_descriptor.h - Newmota compatibility metadata in a native ESP-IDF image

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file image_descriptor.h
 * @brief Newmota compatibility metadata in a native ESP-IDF image.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace newmota::platform::esp_idf {

constexpr std::size_t kImageHeaderSize = 24U;
constexpr std::size_t kImageSegmentHeaderSize = 8U;
constexpr std::size_t kApplicationDescriptionSize = 256U;
constexpr std::size_t kCustomDescriptorOffset =
    kImageHeaderSize + kImageSegmentHeaderSize + kApplicationDescriptionSize;
constexpr std::uint16_t kCustomDescriptorSchema = 1U;
constexpr std::uint8_t kApplicationImageRole = 1U;
constexpr std::uint8_t kSafebootImageRole = 2U;

struct __attribute__((packed)) NewmotaEspImageDescriptor final {
    char magic[8];
    std::uint16_t schema;
    std::uint16_t descriptor_size;
    std::uint8_t image_role;
    std::uint8_t reserved[3];
    std::uint32_t app0_offset;
    std::uint32_t safeboot_offset;
    std::uint32_t otadata_offset;
    std::uint32_t nvs_offset;
    char environment[32];
    char sdk_target[16];
    char layout_id[65];
    std::uint32_t psram_bytes;
};

static_assert(
    sizeof(NewmotaEspImageDescriptor) == 149U, "Newmota ESP image descriptor format changed");
static_assert(kCustomDescriptorOffset == 288U, "ESP-IDF app descriptor offset changed");

} // namespace newmota::platform::esp_idf
