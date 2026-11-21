/*
  flash_access.cpp - checked Pico physical flash operations

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file flash_access.cpp
 * @brief Checked Pico physical flash operations.
 */

#include "flash_access.h"

#include "boot_trace.h"

#include "newmota/platform/flash_capacity.h"

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"

#if PICO_RP2350
#include "pico/bootrom.h"
#elif PICO_RP2040
#include "pico/flash.h"
#else
#error "The Pico flash adapter supports RP2040 and RP2350 targets only"
#endif

#include <array>
#include <cstring>

#ifndef PICO_FLASH_SIZE_BYTES
#error "PICO_FLASH_SIZE_BYTES must describe the selected board flash"
#endif

namespace newmota::platform::pico_detail {
namespace {

/** @brief Addressable flash; the catalog size until flash_capacity() proves more. */
std::uint32_t capacity_bytes = PICO_FLASH_SIZE_BYTES;
bool capacity_resolved = false;

[[nodiscard]] bool valid_range(const std::uint32_t offset, const std::size_t size) {
    return size != 0U && offset <= capacity_bytes && size <= capacity_bytes - offset;
}

#if PICO_RP2040
/** @brief RP2040 XIP and its 3-byte flash commands reach 16 MiB. */
constexpr std::uint32_t kAddressableBytes = 16U * 1024U * 1024U;

struct JedecId final {
    std::array<std::uint8_t, 4> bytes{};
};

void read_jedec_id(void* const context) {
    // flash_do_cmd() takes XIP down, so both buffers must live in RAM.
    std::array<std::uint8_t, 4> command{0x9FU, 0U, 0U, 0U};
    flash_do_cmd(command.data(), static_cast<JedecId*>(context)->bytes.data(), command.size());
}

/** @brief Compares a probe window with offset 0 through the uncached XIP alias. */
[[nodiscard]] bool mirrors_start(const std::uint32_t offset) {
    const auto* const base = reinterpret_cast<const std::uint8_t*>(XIP_NOCACHE_NOALLOC_BASE);
    return std::memcmp(base, base + offset, kFlashMirrorProbeSize) == 0;
}
#endif

#if PICO_RP2350
[[nodiscard]] constexpr cflash_flags_t flash_flags(const std::uint32_t operation) {
    return cflash_flags_t{(CFLASH_ASPACE_VALUE_STORAGE << CFLASH_ASPACE_LSB) |
                          (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB) |
                          (operation << CFLASH_OP_LSB)};
}
#elif PICO_RP2040 && !defined(NEWMOTA_PICO_DIAGNOSTIC_NO_FLASH_WRITES)
struct FlashCommand final {
    std::uint32_t offset;
    std::size_t size;
    const std::uint8_t* data;
    bool erase;
};

void execute_flash_command(void* const context) {
    const auto& command = *static_cast<const FlashCommand*>(context);
    if (command.erase) {
        flash_range_erase(command.offset, command.size);
    } else {
        flash_range_program(command.offset, command.data, command.size);
    }
}

[[nodiscard]] bool execute_flash_command(const FlashCommand& command) {
    // Newmota runs on core 0 only. The SDK disables its interrupts and
    // coordinates with any other core before taking XIP down for the write.
    // On RP2040 the SDK also snapshots boot2 from XIP_BASE to restore XIP.
    // In this layout XIP_BASE contains Safeboot's boot2, so both images must
    // keep using boot2 code compatible with the same physical flash device.
    //
    // The third argument bounds the core-lockout handshake and is expressed in
    // milliseconds, not microseconds. Core 0 declares core 1 unused, so the
    // handshake is skipped entirely and only a short bound is ever reachable.
    const auto result =
        flash_safe_execute(execute_flash_command, const_cast<FlashCommand*>(&command), 10U);
    return result == PICO_OK;
}
#endif

} // namespace

std::uint32_t flash_capacity() {
    if (capacity_resolved)
        return capacity_bytes;
    capacity_resolved = true;
#if PICO_RP2040
    JedecId id{};
    if (flash_safe_execute(read_jedec_id, &id, 10U) == PICO_OK) {
        // Byte 0 is clocked out during the command; bytes 1..3 are maker, type, capacity.
        capacity_bytes = resolve_flash_capacity(
            PICO_FLASH_SIZE_BYTES, id.bytes[3], kAddressableBytes, mirrors_start);
    }
#endif
    return capacity_bytes;
}

bool flash_read(const std::uint32_t offset, void* const buffer, const std::size_t size) {
    if (buffer == nullptr || !valid_range(offset, size))
        return false;
#if PICO_RP2350
    // Avoid the ROM read path's flash-safe critical section and global lock.
    // The uncached, untranslated alias addresses the same physical bytes;
    // hardware_flash flushes cache state after its erase/program operations.
    std::memcpy(
        buffer, reinterpret_cast<const void*>(XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + offset), size);
    return true;
#elif PICO_RP2040
    // Diagnostic: use the ordinary cached physical XIP window. RP2040 has
    // no rolling translation; writes flush the cache through hardware_flash.
    // Keep the copy routine unchanged to isolate the alias used for reads.
    std::memcpy(buffer, reinterpret_cast<const void*>(XIP_BASE + offset), size);
    return true;
#endif
}

bool flash_erase(const std::uint32_t offset, const std::size_t size) {
    if (!valid_range(offset, size) || offset % FLASH_SECTOR_SIZE != 0U ||
        size % FLASH_SECTOR_SIZE != 0U) {
        return false;
    }
#if PICO_RP2350
    return rom_flash_op(flash_flags(CFLASH_OP_VALUE_ERASE),
               static_cast<std::uintptr_t>(XIP_BASE) + offset, size, nullptr) == BOOTROM_OK;
#elif PICO_RP2040
    mark_boot_stage(BootStage::flash_erase_enter);
    const auto erased = execute_flash_command(FlashCommand{offset, size, nullptr, true});
    mark_boot_stage(BootStage::flash_erase_done);
    return erased;
#endif
}

bool flash_program(const std::uint32_t offset, const void* const data, const std::size_t size) {
    if (data == nullptr || !valid_range(offset, size) || offset % FLASH_PAGE_SIZE != 0U ||
        size % FLASH_PAGE_SIZE != 0U) {
        return false;
    }
#if PICO_RP2350
    return rom_flash_op(flash_flags(CFLASH_OP_VALUE_PROGRAM),
               static_cast<std::uintptr_t>(XIP_BASE) + offset, static_cast<std::uint32_t>(size),
               const_cast<std::uint8_t*>(static_cast<const std::uint8_t*>(data))) == BOOTROM_OK;
#elif PICO_RP2040
    mark_boot_stage(BootStage::flash_program_enter);
    const auto programmed = execute_flash_command(
        FlashCommand{offset, size, static_cast<const std::uint8_t*>(data), false});
    mark_boot_stage(BootStage::flash_program_done);
    return programmed;
#endif
}

} // namespace newmota::platform::pico_detail
