/*
  rp2040_image.h - bounded validation for RP2040 boot2 and application images

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

namespace newmota::platform::pico_detail {

using ImageReadAt = bool (*)(void*, std::uint32_t, std::uint8_t*, std::size_t);

namespace rp2040_image_detail {

constexpr std::uint32_t kBoot2Size = 0x100U;
constexpr std::uint32_t kBoot2PayloadSize = kBoot2Size - 4U;
constexpr std::uint32_t kSramBegin = 0x20000000U;
constexpr std::uint32_t kSramEnd = 0x20042000U;

[[nodiscard]] inline std::uint8_t reverse_bits(std::uint8_t value) {
    std::uint8_t reversed = 0U;
    for (unsigned bit = 0U; bit < 8U; ++bit) {
        reversed = static_cast<std::uint8_t>(
            (static_cast<std::uint32_t>(reversed) << 1U) | static_cast<std::uint32_t>(value & 1U));
        value >>= 1U;
    }
    return reversed;
}

[[nodiscard]] inline std::uint32_t reverse_bits(std::uint32_t value) {
    std::uint32_t reversed = 0U;
    for (unsigned bit = 0U; bit < 32U; ++bit) {
        reversed = (reversed << 1U) | (value & 1U);
        value >>= 1U;
    }
    return reversed;
}

/** @brief Reproduces the RP2040 ROM boot2 CRC over its first 252 bytes. */
[[nodiscard]] inline std::uint32_t boot2_checksum(const std::uint8_t* const bytes) {
    // Pico SDK invokes pad_checksum with seed 0xffffffff. Its bit-reversed
    // implementation starts the reflected CRC register at this seed value.
    std::uint32_t crc = UINT32_MAX;
    for (std::uint32_t index = 0U; index < kBoot2PayloadSize; ++index) {
        crc ^= reverse_bits(bytes[index]);
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? 0xEDB88320U : 0U);
        }
    }
    return reverse_bits(crc);
}

[[nodiscard]] inline std::uint32_t read_u32(const std::uint8_t* const bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

[[nodiscard]] inline bool valid_stack_pointer(const std::uint32_t stack_pointer,
    const std::uint32_t sram_begin, const std::uint32_t sram_end) {
    return (stack_pointer & 7U) == 0U && stack_pointer > sram_begin && stack_pointer <= sram_end;
}

[[nodiscard]] inline bool executable_address(
    const std::uint32_t address, const std::uint64_t image_begin, const std::uint64_t image_end) {
    const auto code_address = static_cast<std::uint64_t>(address & ~1U);
    return (address & 1U) != 0U && code_address >= image_begin + kBoot2Size &&
           code_address < image_end;
}

} // namespace rp2040_image_detail

/**
 * @brief Validates an RP2040 image's checksummed boot2 prefix and reset vectors.
 *
 * The reader returns bytes from the image at offset zero. The runtime base is
 * the physical XIP address at which the image is stored, and the executable
 * reset handler must point past the 256-byte boot2 prefix into this image.
 */
[[nodiscard]] inline bool valid_rp2040_application(const std::uint32_t image_size,
    const std::uint32_t runtime_base, const std::uint32_t sram_begin, const std::uint32_t sram_end,
    const ImageReadAt read_at, void* const read_context) {
    using namespace rp2040_image_detail;
    if (read_at == nullptr || image_size < kBoot2Size + 8U || image_size % 4U != 0U ||
        sram_begin >= sram_end ||
        static_cast<std::uint64_t>(runtime_base) + image_size >
            static_cast<std::uint64_t>(UINT32_MAX) + 1U) {
        return false;
    }

    alignas(4) std::array<std::uint8_t, kBoot2Size> boot2{};
    if (!read_at(read_context, 0U, boot2.data(), boot2.size()) ||
        read_u32(boot2.data() + kBoot2PayloadSize) != boot2_checksum(boot2.data())) {
        return false;
    }

    alignas(4) std::array<std::uint8_t, 8U> vectors{};
    if (!read_at(read_context, kBoot2Size, vectors.data(), vectors.size()))
        return false;
    const auto stack_pointer = read_u32(vectors.data());
    const auto reset_handler = read_u32(vectors.data() + 4U);
    const auto image_end = static_cast<std::uint64_t>(runtime_base) + image_size;
    return valid_stack_pointer(stack_pointer, sram_begin, sram_end) &&
           executable_address(reset_handler, runtime_base, image_end);
}

} // namespace newmota::platform::pico_detail
