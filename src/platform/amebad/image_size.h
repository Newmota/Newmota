/*
  image_size.h - bounded AmebaD combined LP/HP image measurement

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace newmota::platform::amebad_detail {

using ImageReadAt = bool (*)(void*, std::uint32_t, std::uint8_t*, std::size_t);
constexpr std::uint32_t kImageHeaderBytes = 32U;

/** @brief Reads a little-endian field from a native image header. */
[[nodiscard]] inline std::uint32_t read_image_u32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

/**
 * @brief Measures the installed, unsigned LP/HP image including core padding.
 *
 * The reader addresses the active slot relative to offset zero. The SDK packs
 * two LP headers and three HP headers (including empty PSRAM), and pads each
 * core to 4 KiB. Secure extensions are not enabled by current build profiles.
 */
[[nodiscard]] inline bool combined_image_size(const std::uint32_t capacity,
    const ImageReadAt read_at, void* const read_context, std::uint32_t& image_size) {
    if (read_at == nullptr)
        return false;
    constexpr std::uint32_t kSectorSize = 4096U;
    std::uint32_t offset = 0U;
    for (unsigned core = 0U; core < 2U; ++core) {
        const unsigned header_count = core == 0U ? 2U : 3U;
        for (unsigned index = 0U; index < header_count; ++index) {
            if (capacity - offset < kImageHeaderBytes)
                return false;
            std::array<std::uint8_t, kImageHeaderBytes> header{};
            if (!read_at(read_context, offset, header.data(), header.size()) ||
                read_image_u32(header.data()) != 0x35393138U ||
                read_image_u32(header.data() + 4U) != 0x31313738U) {
                return false;
            }
            if (core == 1U && index == 0U && read_image_u32(header.data() + 16U) != UINT32_MAX) {
                return false;
            }
            const auto payload_size = read_image_u32(header.data() + 8U);
            if (payload_size > capacity - offset - kImageHeaderBytes)
                return false;
            offset += kImageHeaderBytes + payload_size;
        }
        const auto padding = (kSectorSize - offset % kSectorSize) % kSectorSize;
        if (padding > capacity - offset)
            return false;
        offset += padding;
    }
    image_size = offset;
    return true;
}

} // namespace newmota::platform::amebad_detail
