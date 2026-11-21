/*
  filesystem.cpp - Beken LittleFS block-device adapter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file filesystem.cpp
 * @brief Beken LittleFS block-device adapter on its own flash region.
 */

#include "newmota/platform/block_device.h"

#include "flash_access.h"

#include <cstddef>
#include <cstdint>

#ifndef NEWMOTA_FILESYSTEM_OFFSET
#error "NEWMOTA_FILESYSTEM_OFFSET is required"
#endif

#ifndef NEWMOTA_FILESYSTEM_SIZE
#error "NEWMOTA_FILESYSTEM_SIZE is required"
#endif

#ifndef NEWMOTA_FILESYSTEM_BLOCK_SIZE
#error "NEWMOTA_FILESYSTEM_BLOCK_SIZE is required"
#endif

namespace newmota::platform {
namespace {

constexpr std::uint32_t kFilesystemOffset = NEWMOTA_FILESYSTEM_OFFSET;
constexpr std::uint32_t kFilesystemSize = NEWMOTA_FILESYSTEM_SIZE;
constexpr std::uint32_t kBlockSize = NEWMOTA_FILESYSTEM_BLOCK_SIZE;
constexpr std::uint32_t kTransferSize = 128U;
constexpr std::uint8_t kContext = 0U;

static_assert(kBlockSize == beken::kFlashSectorSize);
static_assert(kFilesystemOffset % kBlockSize == 0U);
static_assert(kFilesystemSize >= 2U * kBlockSize);
static_assert(kFilesystemSize % kBlockSize == 0U);

[[nodiscard]] bool valid_range(const std::uint32_t offset, const std::size_t size) {
    return size != 0U && offset <= kFilesystemSize && size <= kFilesystemSize - offset;
}

bool read_region(
    const void*, const std::uint32_t offset, void* const data, const std::size_t size) {
    return valid_range(offset, size) && beken::read_flash(kFilesystemOffset + offset, data, size);
}

bool program_region(
    const void*, const std::uint32_t offset, const void* const data, const std::size_t size) {
    return valid_range(offset, size) && offset % kTransferSize == 0U &&
           size % kTransferSize == 0U &&
           beken::program_flash(kFilesystemOffset + offset, data, size);
}

bool erase_region(const void*, const std::uint32_t offset, const std::size_t size) {
    return size == kBlockSize && valid_range(offset, size) && offset % kBlockSize == 0U &&
           beken::erase_flash_sector(kFilesystemOffset + offset);
}

bool sync_region(const void*) {
    return true;
}

} // namespace

bool open_filesystem_block_device(BlockDevice& device) {
    device = BlockDevice{&kContext, kFilesystemSize, kTransferSize, kTransferSize, kBlockSize,
        read_region, program_region, erase_region, sync_region};
    return device.valid();
}

} // namespace newmota::platform
