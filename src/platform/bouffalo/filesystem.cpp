/*
  filesystem.cpp - Bouffalo LittleFS block-device adapter for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/block_device.h"
#include "newmota/platform/flash_capacity.h"

extern "C" {
#include "bflb_boot2.h"
#include "bflb_flash.h"
}

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

constexpr std::uint32_t kExpectedOffset = NEWMOTA_FILESYSTEM_OFFSET;
constexpr std::uint32_t kExpectedSize = NEWMOTA_FILESYSTEM_SIZE;
constexpr std::uint32_t kBlockSize = NEWMOTA_FILESYSTEM_BLOCK_SIZE;
/** @brief Matches the factory LittleFS image geometry (128-byte program units). */
constexpr std::uint32_t kTransferSize = 128U;
/** @brief Native BouffaloSDK name of the dedicated Newmota filesystem partition. */
constexpr char kPartitionName[] = "lfs";
/** @brief Catalog flash size that the build layout describes. */
constexpr std::uint32_t kFlashBytes = NEWMOTA_UPDATE_FLASH_BYTES;
/** @brief Bouffalo flash commands use 3-byte addresses. */
constexpr std::uint32_t kAddressableBytes = 16U * 1024U * 1024U;

struct FilesystemContext final {
    std::uint32_t offset{0};
    std::uint32_t size{0};
    bool resolved{false};
};

FilesystemContext context{};

/** @brief Compares a probe window with offset 0; a failed read counts as a mirror. */
[[nodiscard]] bool mirrors_start(const std::uint32_t offset) {
    std::array<std::uint8_t, kFlashMirrorProbeSize> start{};
    std::array<std::uint8_t, kFlashMirrorProbeSize> probe{};
    return bflb_flash_read(0U, start.data(), static_cast<std::uint32_t>(start.size())) != 0 ||
           bflb_flash_read(offset, probe.data(), static_cast<std::uint32_t>(probe.size())) != 0 ||
           start == probe;
}

/**
 * @brief Accepts the build size, or a final region the Newmota flasher grew to the flash end.
 *
 * The table only nominates a larger end; it must be a power of two that the mirror probe
 * proves addressable, so LittleFS can never wrap onto boot2.
 */
[[nodiscard]] bool accepted_size(const std::uint32_t size) {
    if (size == kExpectedSize)
        return true;
    if (kExpectedOffset + kExpectedSize != kFlashBytes || size < kExpectedSize ||
        size > kAddressableBytes - kExpectedOffset) {
        return false;
    }
    const auto end = kExpectedOffset + size;
    if ((end & (end - 1U)) != 0U)
        return false;
    const auto capacity_code = static_cast<std::uint8_t>(__builtin_ctz(end));
    return resolve_flash_capacity(kFlashBytes, capacity_code, kAddressableBytes, mirrors_start) >=
           end;
}

/** @brief Resolves the dedicated LittleFS bounds from the active native table. */
bool resolve_partition() {
    if (context.resolved) {
        return true;
    }
    // bflb_boot2_init reads both CRC-protected table copies and caches the
    // authoritative one; it is read-only and safe to repeat.
    if (bflb_boot2_init() != 0) {
        return false;
    }
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    if (bflb_boot2_partition_addr_active(kPartitionName, &address, &size) != 0) {
        return false;
    }
    // Fail closed when flash contents disagree with the build layout: the
    // filesystem must never silently attach to an unexpected region.
    if (address != kExpectedOffset || !accepted_size(size)) {
        return false;
    }
    if (size < 2U * kBlockSize || size % kBlockSize != 0U || address % kBlockSize != 0U) {
        return false;
    }
    context.offset = address;
    context.size = size;
    context.resolved = true;
    return true;
}

/** @brief Checks bounds against the resolved LittleFS region. */
[[nodiscard]] bool valid_range(const std::uint32_t offset, const std::size_t size) {
    return context.resolved && size != 0U && offset <= context.size &&
           size <= context.size - offset;
}

/** @brief Reads a bounded LittleFS range, preserving native flash errors. */
bool read_flash(
    const void*, const std::uint32_t offset, void* const buffer, const std::size_t size) {
    if (!valid_range(offset, size) || buffer == nullptr) {
        return false;
    }
    return bflb_flash_read(context.offset + offset, static_cast<std::uint8_t*>(buffer),
               static_cast<std::uint32_t>(size)) == 0;
}

/** @brief Programs a bounded LittleFS range, preserving native flash errors. */
bool program_flash(
    const void*, const std::uint32_t offset, const void* const buffer, const std::size_t size) {
    if (!valid_range(offset, size) || buffer == nullptr || offset % kTransferSize != 0U ||
        size % kTransferSize != 0U) {
        return false;
    }
    // bflb_flash_write takes a mutable buffer but never modifies source data
    // held in RAM; the const cast stays within one synchronous call.
    return bflb_flash_write(context.offset + offset,
               const_cast<std::uint8_t*>(static_cast<const std::uint8_t*>(buffer)),
               static_cast<std::uint32_t>(size)) == 0;
}

/** @brief Erases one 4 KiB LittleFS block, preserving native flash errors. */
bool erase_flash(const void*, const std::uint32_t offset, const std::size_t size) {
    if (!valid_range(offset, size) || size != kBlockSize || offset % kBlockSize != 0U) {
        return false;
    }
    return bflb_flash_erase(context.offset + offset, static_cast<std::uint32_t>(size)) == 0;
}

/** @brief Bouffalo flash operations are synchronous. */
bool sync_flash(const void*) {
    return true;
}

} // namespace

/**
 * @brief Opens the resolved dedicated LittleFS region as a block device.
 * @return true when the native `lfs` partition resolved and geometry is valid.
 */
bool open_filesystem_block_device(BlockDevice& device) {
    if (!resolve_partition()) {
        return false;
    }
    // The SDK partition API has no bus-address alias for single-slot data
    // partitions; physical offsets are the correct addressing here because
    // bflb_flash_* takes physical flash addresses.
    static_assert(kBlockSize == 4096U, "Bouffalo erase geometry is 4 KiB");
    static_assert(kExpectedSize >= 2U * kBlockSize, "filesystem needs two blocks");
    static_assert(kExpectedSize % kBlockSize == 0U, "filesystem must be block aligned");
    static_assert(kExpectedOffset % kBlockSize == 0U, "filesystem must be block aligned");
    device = BlockDevice{&context, context.size, kTransferSize, kTransferSize, kBlockSize,
        read_flash, program_flash, erase_flash, sync_flash};
    return device.valid();
}

} // namespace newmota::platform
