/*
  picobin_image.h - bounded validation for RP2350 flash application images

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

namespace picobin_detail {

constexpr std::uint32_t kMarkerStart = 0xFFFFDED3U;
constexpr std::uint32_t kMarkerEnd = 0xAB123579U;
constexpr std::uint8_t kItemImageType = 0x42U;
constexpr std::uint8_t kItemVectorTable = 0x03U;
constexpr std::uint8_t kItemEntryPoint = 0x44U;
constexpr std::uint8_t kItemPartitionTable = 0x0AU;
constexpr std::uint8_t kItemLast = 0x7FU;
constexpr std::uint32_t kMaxImageDefinitionBlockSize = 384U;
constexpr std::size_t kMaxLinkedBlocks = 64U;
constexpr std::size_t kScanWindowSize = 4096U;
constexpr std::size_t kScanChunkSize = 512U;
constexpr std::uint16_t kRp2350ArmSecureExecutable = 0x1021U;

struct BlockInfo final {
    std::uint32_t next_offset{0U};
    std::uint8_t first_item_type{0U};
    bool has_image_type{false};
    std::uint16_t image_type{0U};
    bool has_vector_table{false};
    std::uint32_t vector_table{0U};
    bool has_entry_point{false};
    std::uint32_t entry_pc{0U};
    std::uint32_t entry_sp{0U};
};

[[nodiscard]] inline std::uint16_t read_u16(const std::uint8_t* const bytes) {
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
}

[[nodiscard]] inline std::uint32_t read_u32(const std::uint8_t* const bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

[[nodiscard]] inline bool record_unique(
    const bool already_set, const std::uint32_t old_value, const std::uint32_t new_value) {
    return !already_set || old_value == new_value;
}

/** @brief Parses one bounded IMAGE_DEF-style block and its relative link. */
[[nodiscard]] inline bool parse_block(const std::uint32_t image_size, const std::uint32_t offset,
    const ImageReadAt read_at, void* const read_context, BlockInfo& info) {
    if (read_at == nullptr || offset % 4U != 0U || offset > image_size ||
        image_size - offset < 20U) {
        return false;
    }

    alignas(4) std::array<std::uint8_t, kMaxImageDefinitionBlockSize> bytes{};
    const auto available = static_cast<std::size_t>(image_size - offset);
    const auto read_size = (available < bytes.size() ? available : bytes.size()) & ~3U;
    if (read_size < 20U || !read_at(read_context, offset, bytes.data(), read_size) ||
        read_u32(bytes.data()) != kMarkerStart) {
        return false;
    }

    std::size_t cursor = 4U;
    std::uint32_t preceding_words = 0U;
    bool first_item = true;
    while (cursor + 4U <= read_size) {
        const std::uint8_t raw_type = bytes[cursor];
        const std::uint8_t item_type = raw_type & 0x7FU;
        if (raw_type == 0xFFU && item_type == kItemLast) {
            if (read_u16(bytes.data() + cursor + 1U) != preceding_words ||
                bytes[cursor + 3U] != 0U || cursor + 12U > read_size ||
                read_u32(bytes.data() + cursor + 8U) != kMarkerEnd) {
                return false;
            }
            const std::uint32_t relative_link = read_u32(bytes.data() + cursor + 4U);
            const auto target =
                static_cast<std::int64_t>(offset) + static_cast<std::int32_t>(relative_link);
            const auto block_size = cursor + 12U;
            if (target < 0 || target >= image_size || target % 4 != 0 ||
                block_size > kMaxImageDefinitionBlockSize || block_size % 4U != 0U) {
                return false;
            }
            info.next_offset = static_cast<std::uint32_t>(target);
            return true;
        }

        const bool two_byte_size = (raw_type & 0x80U) != 0U;
        const std::size_t size_header = two_byte_size ? 3U : 2U;
        if (cursor + size_header > read_size)
            return false;
        const std::uint32_t item_words =
            two_byte_size ? read_u16(bytes.data() + cursor + 1U) : bytes[cursor + 1U];
        if (item_words == 0U || item_words > (read_size - cursor) / 4U)
            return false;
        const std::size_t item_size = static_cast<std::size_t>(item_words) * 4U;
        if (item_size < size_header || cursor + item_size > read_size ||
            item_type == kItemPartitionTable) {
            return false;
        }

        const bool is_first_item = first_item;
        if (is_first_item) {
            info.first_item_type = item_type;
            first_item = false;
        }
        if (item_type == kItemImageType) {
            if (!is_first_item || two_byte_size || item_words != 1U)
                return false;
            const auto value = read_u16(bytes.data() + cursor + 2U);
            if (!record_unique(info.has_image_type, info.image_type, value))
                return false;
            info.has_image_type = true;
            info.image_type = value;
        } else if (item_type == kItemVectorTable) {
            if (two_byte_size || item_words != 2U || bytes[cursor + 2U] != 0U ||
                bytes[cursor + 3U] != 0U) {
                return false;
            }
            const auto address = read_u32(bytes.data() + cursor + 4U);
            if (!record_unique(info.has_vector_table, info.vector_table, address)) {
                return false;
            }
            info.has_vector_table = true;
            info.vector_table = address;
        } else if (item_type == kItemEntryPoint) {
            if (two_byte_size || item_words != 3U || bytes[cursor + 2U] != 0U ||
                bytes[cursor + 3U] != 0U) {
                return false;
            }
            const auto pc = read_u32(bytes.data() + cursor + 4U);
            const auto sp = read_u32(bytes.data() + cursor + 8U);
            if (!record_unique(info.has_entry_point, info.entry_pc, pc) ||
                !record_unique(info.has_entry_point, info.entry_sp, sp)) {
                return false;
            }
            info.has_entry_point = true;
            info.entry_pc = pc;
            info.entry_sp = sp;
        }

        preceding_words += item_words;
        cursor += item_size;
    }
    return false;
}

[[nodiscard]] inline bool executable_address(
    const std::uint32_t address, const std::uint64_t image_begin, const std::uint64_t image_end) {
    const std::uint64_t code_address = static_cast<std::uint64_t>(address & ~1U);
    return (address & 1U) != 0U && code_address >= image_begin && code_address < image_end;
}

[[nodiscard]] inline bool valid_stack_pointer(const std::uint32_t stack_pointer,
    const std::uint32_t sram_begin, const std::uint32_t sram_end) {
    return (stack_pointer & 7U) == 0U && stack_pointer > sram_begin && stack_pointer <= sram_end;
}

} // namespace picobin_detail

/**
 * @brief Validates the RP2350 ARM-S image metadata loop and launch addresses.
 *
 * The reader must return bytes from the candidate application image. Validation
 * rejects malformed blocks, non-ARM-S RP2350 executables, partition-table blocks,
 * broken loops, and vectors or entry points outside this image and RP2350 SRAM.
 */
[[nodiscard]] inline bool valid_rp2350_application(const std::uint32_t image_size,
    const std::uint32_t runtime_base, const std::uint32_t sram_begin, const std::uint32_t sram_end,
    const ImageReadAt read_at, void* const read_context) {
    using namespace picobin_detail;
    if (image_size < 20U || image_size % 4U != 0U || read_at == nullptr || sram_begin >= sram_end ||
        static_cast<std::uint64_t>(runtime_base) + image_size >
            static_cast<std::uint64_t>(UINT32_MAX) + 1U) {
        return false;
    }

    std::uint32_t first_offset = image_size;
    const auto scan_size =
        static_cast<std::size_t>(image_size < kScanWindowSize ? image_size : kScanWindowSize) & ~3U;
    alignas(4) std::array<std::uint8_t, kScanChunkSize> scan{};
    for (std::size_t base = 0U; base < scan_size; base += scan.size()) {
        const auto count = scan_size - base < scan.size() ? scan_size - base : scan.size();
        if (!read_at(read_context, static_cast<std::uint32_t>(base), scan.data(), count)) {
            return false;
        }
        for (std::size_t offset = 0U; offset + 4U <= count; offset += 4U) {
            if (read_u32(scan.data() + offset) == kMarkerStart) {
                first_offset = static_cast<std::uint32_t>(base + offset);
                break;
            }
        }
        if (first_offset != image_size)
            break;
    }
    if (first_offset == image_size)
        return false;

    std::array<std::uint32_t, kMaxLinkedBlocks> visited{};
    visited[0] = first_offset;
    std::size_t visited_count = 1U;
    std::uint32_t current_offset = first_offset;
    bool first_block = true;
    bool has_vector_table = false;
    std::uint32_t vector_table = 0U;
    bool has_entry_point = false;
    std::uint32_t entry_pc = 0U;
    std::uint32_t entry_sp = 0U;

    while (true) {
        BlockInfo info{};
        if (!parse_block(image_size, current_offset, read_at, read_context, info) ||
            (first_block && (info.first_item_type != kItemImageType || !info.has_image_type)) ||
            (info.has_image_type && info.image_type != kRp2350ArmSecureExecutable)) {
            return false;
        }
        if (info.has_vector_table) {
            if (!record_unique(has_vector_table, vector_table, info.vector_table)) {
                return false;
            }
            has_vector_table = true;
            vector_table = info.vector_table;
        }
        if (info.has_entry_point) {
            if (has_entry_point && (entry_pc != info.entry_pc || entry_sp != info.entry_sp)) {
                return false;
            }
            has_entry_point = true;
            entry_pc = info.entry_pc;
            entry_sp = info.entry_sp;
        }

        if (info.next_offset == first_offset)
            break;
        if (info.next_offset <= first_offset || info.next_offset >= image_size ||
            visited_count == visited.size()) {
            return false;
        }
        for (std::size_t index = 0U; index < visited_count; ++index) {
            if (visited[index] == info.next_offset)
                return false;
        }
        visited[visited_count++] = info.next_offset;
        current_offset = info.next_offset;
        first_block = false;
    }

    const std::uint64_t image_begin = runtime_base;
    const std::uint64_t image_end = image_begin + image_size;
    if (has_entry_point && (!executable_address(entry_pc, image_begin, image_end) ||
                               !valid_stack_pointer(entry_sp, sram_begin, sram_end))) {
        return false;
    }

    if (has_vector_table || !has_entry_point) {
        const std::uint32_t vector_address = has_vector_table ? vector_table : runtime_base;
        if (vector_address % 4U != 0U || vector_address < runtime_base ||
            static_cast<std::uint64_t>(vector_address) + 8U > image_end) {
            return false;
        }
        alignas(4) std::array<std::uint8_t, 8U> vectors{};
        if (!read_at(read_context, vector_address - runtime_base, vectors.data(), vectors.size())) {
            return false;
        }
        const auto stack_pointer = read_u32(vectors.data());
        const auto reset_handler = read_u32(vectors.data() + 4U);
        if (!valid_stack_pointer(stack_pointer, sram_begin, sram_end) ||
            !executable_address(reset_handler, image_begin, image_end)) {
            return false;
        }
    }
    return true;
}

} // namespace newmota::platform::pico_detail
