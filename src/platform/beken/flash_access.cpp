/*
  flash_access.cpp - checked physical-flash operations for Beken adapters

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file flash_access.cpp
 * @brief Checked physical-flash operations for Beken adapters.
 */

#include "flash_access.h"

#include "newmota/platform/beken/update_config.h"

#include <array>
#include <cstddef>
#include <cstdint>

extern "C" {
#include "BkDriverFlash.h"
#include "flash_pub.h"

// BkDriverFlash.c exports these guards, but the BDK header only declares the
// init entry point.  Keep the declarations local to this adapter rather than
// modifying the immutable SDK checkout.
int hal_flash_lock(void);
int hal_flash_unlock(void);
}

namespace newmota::platform::beken {
namespace {

constexpr std::size_t kVerifyChunkSize = 128U;
std::array<std::uint8_t, kVerifyChunkSize> verify_scratch;
std::array<std::uint8_t, kFlashSectorSize> sector_source;
std::array<std::uint8_t, kFlashSectorSize> sector_destination;
bool ota_staging_reserved{};

class FlashLock final {
  public:
    FlashLock() : locked_(hal_flash_lock() == kNoErr) {}

    ~FlashLock() {
        if (locked_) {
            (void)hal_flash_unlock();
        }
    }

    [[nodiscard]] bool locked() const { return locked_; }

  private:
    bool locked_;
};

[[nodiscard]] bool read_locked(
    const std::uint32_t address, void* const data, const std::size_t size) {
    return data != nullptr && size != 0U && size <= UINT32_MAX &&
           address <= UINT32_MAX - static_cast<std::uint32_t>(size) &&
           bk_flash_abs_addr_read(
               address, static_cast<unsigned char*>(data), static_cast<UINT32>(size)) == kNoErr;
}

[[nodiscard]] bool valid_range(const std::uint32_t address, const std::size_t size,
    const std::uint32_t region_offset, const std::uint32_t region_size) {
    if (size == 0U || size > UINT32_MAX ||
        address > UINT32_MAX - static_cast<std::uint32_t>(size)) {
        return false;
    }
    const auto end = static_cast<std::uint64_t>(address) + size;
    const auto region_end = static_cast<std::uint64_t>(region_offset) + region_size;
    return address >= region_offset && end <= region_end;
}

[[nodiscard]] bool overlaps(const std::uint32_t address, const std::size_t size,
    const std::uint32_t region_offset, const std::uint32_t region_size) {
    if (size == 0U || size > UINT32_MAX ||
        address > UINT32_MAX - static_cast<std::uint32_t>(size)) {
        return true;
    }
    const auto end = static_cast<std::uint64_t>(address) + size;
    const auto region_end = static_cast<std::uint64_t>(region_offset) + region_size;
    return address < region_end && region_offset < end;
}

[[nodiscard]] bool general_write_allowed_locked(
    const std::uint32_t address, const std::size_t size) {
    return !ota_staging_reserved ||
           !overlaps(address, size, update_config::kOta.offset, update_config::kOta.size);
}

[[nodiscard]] bool verify_locked(
    const std::uint32_t address, const std::uint8_t* const expected, const std::size_t size) {
    std::size_t position = 0U;
    while (position < size) {
        const auto remaining = size - position;
        const auto count = remaining < verify_scratch.size() ? remaining : verify_scratch.size();
        if (!read_locked(
                address + static_cast<std::uint32_t>(position), verify_scratch.data(), count)) {
            return false;
        }
        for (std::size_t index = 0; index < count; ++index) {
            if (verify_scratch[index] != expected[position + index]) {
                return false;
            }
        }
        position += count;
    }
    return true;
}

[[nodiscard]] bool erased_locked(const std::uint32_t address, const std::size_t size) {
    std::size_t position = 0U;
    while (position < size) {
        const auto remaining = size - position;
        const auto count = remaining < verify_scratch.size() ? remaining : verify_scratch.size();
        if (!read_locked(
                address + static_cast<std::uint32_t>(position), verify_scratch.data(), count)) {
            return false;
        }
        for (std::size_t index = 0; index < count; ++index) {
            if (verify_scratch[index] != 0xffU) {
                return false;
            }
        }
        position += count;
    }
    return true;
}

[[nodiscard]] bool program_locked(
    const std::uint32_t address, const void* const data, const std::size_t size) {
    // The BDK absolute primitives mask interrupts during the flash transaction
    // and restore protection afterward. Keep readback verification because the
    // SDK does not propagate all low-level driver failures.
    const bool programmed =
        bk_flash_abs_addr_write(address, static_cast<const unsigned char*>(data),
            static_cast<UINT32>(size), 0) == kNoErr;
    return programmed && verify_locked(address, static_cast<const std::uint8_t*>(data), size);
}

[[nodiscard]] bool erase_locked(const std::uint32_t address) {
    const bool erased = bk_flash_abs_addr_erase(address, kFlashSectorSize) == kNoErr;
    return erased && erased_locked(address, kFlashSectorSize);
}

} // namespace

bool read_flash(const std::uint32_t address, void* const data, const std::size_t size) {
    FlashLock lock;
    return lock.locked() && read_locked(address, data, size);
}

bool program_flash(const std::uint32_t address, const void* const data, const std::size_t size) {
    if (data == nullptr || size == 0U || size > UINT32_MAX ||
        address > UINT32_MAX - static_cast<std::uint32_t>(size)) {
        return false;
    }
    FlashLock lock;
    return lock.locked() && general_write_allowed_locked(address, size) &&
           program_locked(address, data, size);
}

bool erase_flash_sector(const std::uint32_t address) {
    if (address % kFlashSectorSize != 0U) {
        return false;
    }
    FlashLock lock;
    return lock.locked() && general_write_allowed_locked(address, kFlashSectorSize) &&
           erase_locked(address);
}

bool mutate_flash_sector(
    const std::uint32_t address, const SectorMutator mutator, void* const context) {
    if (address % kFlashSectorSize != 0U || mutator == nullptr) {
        return false;
    }
    FlashLock lock;
    if (!lock.locked() || !general_write_allowed_locked(address, kFlashSectorSize) ||
        !read_locked(address, sector_source.data(), sector_source.size())) {
        return false;
    }
    const auto result = mutator(
        context, sector_source.data(), sector_destination.data(), sector_destination.size());
    if (result == SectorMutationResult::failed) {
        return false;
    }
    if (result == SectorMutationResult::unchanged) {
        return true;
    }
    if (!erase_locked(address)) {
        return false;
    }
    return program_locked(address, sector_destination.data(), sector_destination.size());
}

bool begin_ota_staging() {
    FlashLock lock;
    if (!lock.locked() || ota_staging_reserved) {
        return false;
    }
    ota_staging_reserved = true;
    return true;
}

bool release_ota_staging() {
    FlashLock lock;
    if (!lock.locked() || !ota_staging_reserved)
        return false;
    ota_staging_reserved = false;
    return true;
}

bool ota_staging_available() {
    FlashLock lock;
    return lock.locked() && !ota_staging_reserved;
}

bool read_ota_staging(const std::uint32_t address, void* const data, const std::size_t size) {
    FlashLock lock;
    return lock.locked() && ota_staging_reserved &&
           valid_range(address, size, update_config::kOta.offset, update_config::kOta.size) &&
           read_locked(address, data, size);
}

bool program_ota_staging(
    const std::uint32_t address, const void* const data, const std::size_t size) {
    if (data == nullptr || size == 0U)
        return false;
    FlashLock lock;
    return lock.locked() && ota_staging_reserved &&
           valid_range(address, size, update_config::kOta.offset, update_config::kOta.size) &&
           program_locked(address, data, size);
}

bool erase_ota_staging_sector(const std::uint32_t address) {
    if (address % kFlashSectorSize != 0U)
        return false;
    FlashLock lock;
    return lock.locked() && ota_staging_reserved &&
           valid_range(
               address, kFlashSectorSize, update_config::kOta.offset, update_config::kOta.size) &&
           erase_locked(address);
}

} // namespace newmota::platform::beken
