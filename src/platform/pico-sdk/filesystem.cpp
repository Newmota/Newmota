/*
  filesystem.cpp - Pico SDK LittleFS block-device adapter for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/block_device.h"
#include "boot_trace.h"
#include "flash_access.h"

#include "hardware/flash.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifndef NEWMOTA_FILESYSTEM_OFFSET
#error "NEWMOTA_FILESYSTEM_OFFSET is required"
#endif

#ifndef NEWMOTA_FILESYSTEM_SIZE
#error "NEWMOTA_FILESYSTEM_SIZE is required"
#endif

#ifndef NEWMOTA_FILESYSTEM_BLOCK_SIZE
#error "NEWMOTA_FILESYSTEM_BLOCK_SIZE is required"
#endif

#ifndef NEWMOTA_APPLICATION_OFFSET
#error "NEWMOTA_APPLICATION_OFFSET is required"
#endif

#ifndef NEWMOTA_APPLICATION_SIZE
#error "NEWMOTA_APPLICATION_SIZE is required"
#endif

#ifndef NEWMOTA_UPDATE_FLASH_BYTES
#error "NEWMOTA_UPDATE_FLASH_BYTES is required"
#endif

#ifndef NEWMOTA_SAFEBOOT_OFFSET
#error "NEWMOTA_SAFEBOOT_OFFSET is required"
#endif

#ifndef NEWMOTA_SAFEBOOT_SIZE
#error "NEWMOTA_SAFEBOOT_SIZE is required"
#endif

#ifndef NEWMOTA_BOOT_MARKER_OFFSET
#error "NEWMOTA_BOOT_MARKER_OFFSET is required"
#endif

namespace newmota::platform {
namespace {

constexpr std::uint32_t kExpectedOffset = NEWMOTA_FILESYSTEM_OFFSET;
constexpr std::uint32_t kExpectedSize = NEWMOTA_FILESYSTEM_SIZE;
constexpr std::uint32_t kBlockSize = NEWMOTA_FILESYSTEM_BLOCK_SIZE;
/** @brief Logical LittleFS program unit used by the generated image. */
constexpr std::uint32_t kProgramSize = 128U;
/** @brief Native Pico flash page program unit. */
constexpr std::uint32_t kTransferSize = FLASH_PAGE_SIZE;
/** @brief Capacity resolved from the selected Pico board. */
constexpr std::uint32_t kFlashBytes = NEWMOTA_UPDATE_FLASH_BYTES;
/** @brief Reserved sticky-Safeboot marker sector. */
constexpr std::uint32_t kBootMarkerOffset = NEWMOTA_BOOT_MARKER_OFFSET;

struct FilesystemContext final {
    std::uint32_t offset{0};
    std::uint32_t size{0};
    bool resolved{false};
};

FilesystemContext context{};

/** @brief Checks bounds against the resolved LittleFS region. */
[[nodiscard]] bool valid_range(const std::uint32_t offset, const std::size_t size) {
    return context.resolved && size != 0U && offset <= context.size &&
           size <= context.size - offset;
}

/** @brief Reads a bounded LittleFS range through the target flash adapter. */
bool read_flash(
    const void*, const std::uint32_t offset, void* const buffer, const std::size_t size) {
    if (!valid_range(offset, size) || buffer == nullptr) {
        return false;
    }
    pico_detail::mark_boot_stage(pico_detail::BootStage::filesystem_read);
    return pico_detail::flash_read(context.offset + offset, buffer, size);
}

/**
 * @brief Programs a bounded LittleFS range through the Pico page API.
 *
 * LittleFS uses 128-byte program units in the shared factory image, while
 * The checked RP2350 ROM API and RP2040 SDK API both require complete
 * 256-byte flash pages. Read and overlay each affected page so the portable
 * LittleFS geometry remains 128 bytes on both chips.
 */
bool program_flash(
    const void*, const std::uint32_t offset, const void* const buffer, const std::size_t size) {
    if (!valid_range(offset, size) || buffer == nullptr || offset % kProgramSize != 0U ||
        size % kProgramSize != 0U) {
        return false;
    }

    static_assert(
        kTransferSize % kProgramSize == 0U, "LittleFS program unit must divide the flash page");
    alignas(std::uint32_t) std::array<std::uint8_t, kTransferSize> page{};
    const auto* data = static_cast<const std::uint8_t*>(buffer);
    std::size_t done = 0U;
    while (done < size) {
        const auto logical_offset = offset + static_cast<std::uint32_t>(done);
        const auto page_offset = logical_offset & ~(kTransferSize - 1U);
        const auto page_inner = logical_offset - page_offset;
        const auto remaining_page = static_cast<std::size_t>(kTransferSize - page_inner);
        const auto transfer =
            std::min(static_cast<std::size_t>(kProgramSize), std::min(remaining_page, size - done));

        // Preserve the other half of a physical page.  LittleFS has already
        // erased the containing block, but a single logical write can target
        // either half of a page.
        if (!pico_detail::flash_read(context.offset + page_offset, page.data(), page.size())) {
            return false;
        }
        std::memcpy(page.data() + page_inner, data + done, transfer);
        if (!pico_detail::flash_program(context.offset + page_offset, page.data(), page.size())) {
            return false;
        }
        done += transfer;
    }
    return true;
}

/** @brief Erases one 4 KiB LittleFS block. */
bool erase_flash(const void*, const std::uint32_t offset, const std::size_t size) {
    if (!valid_range(offset, size) || size != kBlockSize || offset % kBlockSize != 0U) {
        return false;
    }
    return pico_detail::flash_erase(context.offset + offset, kBlockSize);
}

/** @brief Pico flash operations are synchronous. */
bool sync_flash(const void*) {
    return true;
}

} // namespace

/**
 * @brief Opens the resolved dedicated LittleFS region as a block device.
 *
 * When the filesystem is the last catalog region, it extends to the end of the proven flash.
 * @return true when the build geometry is self-consistent and slot-safe.
 */
bool open_filesystem_block_device(BlockDevice& device) {
    // The layout resolver owns these bounds; refuse a misconfigured build
    // where the filesystem would overlap either firmware slot or flash.
    static_assert(kExpectedOffset % kBlockSize == 0U, "filesystem must be aligned");
    static_assert(kExpectedSize % kBlockSize == 0U, "filesystem must be sized");
    static_assert(kBlockSize == FLASH_SECTOR_SIZE, "Pico erase geometry is 4 KiB");
    static_assert(kTransferSize == FLASH_PAGE_SIZE, "Pico flash page is 256 B");
    static_assert(kExpectedOffset >= NEWMOTA_SAFEBOOT_OFFSET + NEWMOTA_SAFEBOOT_SIZE,
        "filesystem must not overlap the Safeboot slot");
    static_assert(kExpectedOffset >= NEWMOTA_APPLICATION_OFFSET + NEWMOTA_APPLICATION_SIZE,
        "filesystem must not overlap the application slot");
    static_assert(kExpectedOffset >= kBootMarkerOffset + FLASH_SECTOR_SIZE,
        "filesystem must start after the Safeboot marker sector");
    static_assert(kExpectedOffset + kExpectedSize <= kFlashBytes, "filesystem must fit its flash");
    static_assert(kExpectedSize >= 2U * kBlockSize, "filesystem needs two blocks");
    // A filesystem that ends the catalog layout also claims proven flash beyond it; the shared
    // mount grows the volume to this size and keeps its files.
    constexpr bool kFilesystemLast = kExpectedOffset + kExpectedSize == kFlashBytes;
    const auto capacity = pico_detail::flash_capacity();
    context.offset = kExpectedOffset;
    context.size =
        kFilesystemLast && capacity > kFlashBytes ? capacity - kExpectedOffset : kExpectedSize;
    context.resolved = true;
    device = BlockDevice{&context, context.size, 1U, kProgramSize, kBlockSize, read_flash,
        program_flash, erase_flash, sync_flash};
    if (device.valid()) {
        pico_detail::mark_boot_stage(pico_detail::BootStage::filesystem_mapped);
    }
    return device.valid();
}

} // namespace newmota::platform
