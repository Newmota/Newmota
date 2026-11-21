/*
  filesystem.cpp - AmebaD LittleFS block-device adapter for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/block_device.h"

#include "FreeRTOS.h"
#include "device_lock.h"
#include "flash_api.h"
#include "objects.h"

#include "newmota/platform/flash_capacity.h"

#include <array>
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

#ifndef NEWMOTA_UPDATE_FLASH_BYTES
#error "NEWMOTA_UPDATE_FLASH_BYTES is required"
#endif

namespace newmota::platform {
namespace {

constexpr std::uint32_t kFilesystemOffset = NEWMOTA_FILESYSTEM_OFFSET;
constexpr std::uint32_t kFilesystemSize = NEWMOTA_FILESYSTEM_SIZE;
constexpr std::uint32_t kBlockSize = NEWMOTA_FILESYSTEM_BLOCK_SIZE;
constexpr std::uint32_t kProgramSize = 128;
constexpr std::uint8_t kContext = 0;
/** @brief Catalog flash size that the partition layout describes. */
constexpr std::uint32_t kFlashBytes = NEWMOTA_UPDATE_FLASH_BYTES;
/** @brief AmebaD flash commands use 3-byte addresses. */
constexpr std::uint32_t kAddressableBytes = 16U * 1024U * 1024U;

/** @brief Resolved LittleFS size; the catalog size unless proven flash extends it. */
std::uint32_t filesystem_size = kFilesystemSize;

[[nodiscard]] bool valid_range(const std::uint32_t offset, const std::size_t size) {
    return size != 0 && offset <= filesystem_size && size <= filesystem_size - offset;
}

/** @brief Compares a probe window with offset 0; a failed read counts as a mirror. */
[[nodiscard]] bool mirrors_start(const std::uint32_t offset) {
    std::array<std::uint8_t, kFlashMirrorProbeSize> start{};
    std::array<std::uint8_t, kFlashMirrorProbeSize> probe{};
    flash_t flash{};
    device_mutex_lock(RT_DEV_LOCK_FLASH);
    const bool read = flash_stream_read(&flash, 0U, start.size(), start.data()) == 1 &&
                      flash_stream_read(&flash, offset, probe.size(), probe.data()) == 1;
    device_mutex_unlock(RT_DEV_LOCK_FLASH);
    return !read || start == probe;
}

/** @brief Returns the flash size proven by the JEDEC ID and the mirror probe. */
[[nodiscard]] std::uint32_t flash_capacity() {
    std::array<std::uint8_t, 3> id{};
    flash_t flash{};
    device_mutex_lock(RT_DEV_LOCK_FLASH);
    flash_read_id(&flash, id.data(), static_cast<std::uint8_t>(id.size()));
    device_mutex_unlock(RT_DEV_LOCK_FLASH);
    return resolve_flash_capacity(kFlashBytes, id[2], kAddressableBytes, mirrors_start);
}

/** @brief Reads a bounded LittleFS range through the AmebaD flash API. */
bool read_flash(
    const void*, const std::uint32_t offset, void* const buffer, const std::size_t size) {
    if (!valid_range(offset, size)) {
        return false;
    }
    flash_t flash{};
    device_mutex_lock(RT_DEV_LOCK_FLASH);
    const int result = flash_stream_read(&flash, kFilesystemOffset + offset,
        static_cast<std::uint32_t>(size), static_cast<std::uint8_t*>(buffer));
    device_mutex_unlock(RT_DEV_LOCK_FLASH);
    return result == 1;
}

/** @brief Programs a bounded LittleFS range through the AmebaD flash API. */
bool program_flash(
    const void*, const std::uint32_t offset, const void* const buffer, const std::size_t size) {
    if (!valid_range(offset, size) || offset % kProgramSize != 0 || size % kProgramSize != 0) {
        return false;
    }
    flash_t flash{};
    device_mutex_lock(RT_DEV_LOCK_FLASH);
    const int result =
        flash_stream_write(&flash, kFilesystemOffset + offset, static_cast<std::uint32_t>(size),
            const_cast<std::uint8_t*>(static_cast<const std::uint8_t*>(buffer)));
    device_mutex_unlock(RT_DEV_LOCK_FLASH);
    return result == 1;
}

/** @brief Erases one 4 KiB LittleFS block through the AmebaD flash API. */
bool erase_flash(const void*, const std::uint32_t offset, const std::size_t size) {
    if (size != kBlockSize || !valid_range(offset, size) || offset % kBlockSize != 0) {
        return false;
    }
    flash_t flash{};
    device_mutex_lock(RT_DEV_LOCK_FLASH);
    flash_erase_sector(&flash, kFilesystemOffset + offset);
    device_mutex_unlock(RT_DEV_LOCK_FLASH);
    return true;
}

/** @brief AmebaD flash operations are synchronous. */
bool sync_flash(const void*) {
    return true;
}

} // namespace

/**
 * @brief Opens the LittleFS region from the AmebaD partition layout.
 *
 * When the filesystem is the last catalog region, it extends to the end of the proven flash;
 * the shared mount grows the volume to this size and keeps its files.
 * @return true when the Newmota block-device geometry is valid.
 */
bool open_filesystem_block_device(BlockDevice& device) {
    static_assert(kFilesystemOffset + kFilesystemSize <= kFlashBytes, "filesystem must fit");
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        const auto capacity = flash_capacity();
        if (kFilesystemOffset + kFilesystemSize == kFlashBytes && capacity > kFlashBytes)
            filesystem_size = capacity - kFilesystemOffset;
    }
    device = BlockDevice{&kContext, filesystem_size, kProgramSize, kProgramSize, kBlockSize,
        read_flash, program_flash, erase_flash, sync_flash};
    return device.valid();
}

} // namespace newmota::platform
