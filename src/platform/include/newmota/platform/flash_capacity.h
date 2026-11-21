/*
  flash_capacity.h - proven physical flash capacity beyond the catalog size

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstdint>

namespace newmota::platform {

/** @brief Number of bytes compared at offset 0 and at each mirror candidate. */
constexpr std::uint32_t kFlashMirrorProbeSize = 256U;

/**
 * @brief Resolves how much flash may be addressed without wrapping.
 *
 * Serial NOR flash ignores address bits above its capacity, so an access past the real end
 * silently reaches offset 0 (the bootloader). The JEDEC capacity code only nominates a size; a
 * candidate is accepted only when no smaller power of two reads back as a mirror of offset 0.
 * Any doubt yields the catalog size, never more.
 *
 * @param declared_bytes Catalog flash size; a power of two that the result never undercuts.
 * @param jedec_capacity Third JEDEC ID byte, log2 of the size in bytes for common vendors.
 * @param limit_bytes Largest size the platform's flash commands can address.
 * @param mirrors Returns true when offset reads the same probe bytes as offset 0, or on any
 * read failure.
 * @return declared_bytes, or a larger power of two up to limit_bytes proven not to wrap.
 */
template <typename Mirrors>
[[nodiscard]] std::uint32_t resolve_flash_capacity(const std::uint32_t declared_bytes,
    const std::uint8_t jedec_capacity, const std::uint32_t limit_bytes, Mirrors&& mirrors) {
    const auto power_of_two = [](const std::uint32_t value) {
        return value != 0U && (value & (value - 1U)) == 0U;
    };
    if (!power_of_two(declared_bytes) || !power_of_two(limit_bytes) ||
        declared_bytes < 2U * kFlashMirrorProbeSize || jedec_capacity >= 32U) {
        return declared_bytes;
    }
    std::uint32_t candidate = std::uint32_t{1} << jedec_capacity;
    if (candidate > limit_bytes)
        candidate = limit_bytes;
    if (candidate <= declared_bytes)
        return declared_bytes;
    // A real size R mirrors offset 0 at R; the first mirror bounds the usable flash.
    for (std::uint32_t size = declared_bytes; size < candidate; size *= 2U) {
        if (mirrors(size))
            return size;
    }
    return candidate;
}

} // namespace newmota::platform
