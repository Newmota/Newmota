/*
  net_param_tlv.cpp - bounded Beken NET parameter TLV transformations

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file net_param_tlv.cpp
 * @brief Bounded Beken NET parameter TLV transformations.
 */

#include "net_param_tlv.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace newmota::platform::beken {
namespace {

constexpr std::size_t kHeaderSize = 8U;

struct TableInfo final {
    std::size_t end{};
    std::size_t record_offset{};
    std::size_t record_size{};
    bool table_present{};
    bool record_present{};
};

[[nodiscard]] std::uint32_t read_u32(const std::uint8_t* const source) {
    return static_cast<std::uint32_t>(source[0]) | static_cast<std::uint32_t>(source[1]) << 8U |
           static_cast<std::uint32_t>(source[2]) << 16U |
           static_cast<std::uint32_t>(source[3]) << 24U;
}

void write_u32(std::uint8_t* const destination, const std::uint32_t value) {
    destination[0] = static_cast<std::uint8_t>(value);
    destination[1] = static_cast<std::uint8_t>(value >> 8U);
    destination[2] = static_cast<std::uint8_t>(value >> 16U);
    destination[3] = static_cast<std::uint8_t>(value >> 24U);
}

[[nodiscard]] bool erased(const std::uint8_t* const data, const std::size_t size) {
    for (std::size_t index = 0; index < size; ++index) {
        if (data[index] != 0xffU) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool inspect(const std::uint8_t* const sector, const std::size_t sector_size,
    const std::uint32_t record_type, TableInfo& info) {
    info = {};
    if (sector == nullptr || sector_size < 2U * kHeaderSize) {
        return false;
    }
    if (read_u32(sector) != kNetParamTableType) {
        return erased(sector, sector_size);
    }

    const auto payload_size = static_cast<std::size_t>(read_u32(sector + 4U));
    if (payload_size > sector_size - kHeaderSize) {
        return false;
    }
    info.table_present = true;
    info.end = kHeaderSize + payload_size;
    std::size_t position = kHeaderSize;
    while (position < info.end) {
        if (info.end - position < kHeaderSize) {
            return false;
        }
        const auto type = read_u32(sector + position);
        const auto size = static_cast<std::size_t>(read_u32(sector + position + 4U));
        if (size > info.end - position - kHeaderSize) {
            return false;
        }
        const auto whole_size = kHeaderSize + size;
        if (type == record_type) {
            if (info.record_present) {
                return false;
            }
            info.record_present = true;
            info.record_offset = position;
            info.record_size = whole_size;
        }
        position += whole_size;
    }
    return position == info.end;
}

[[nodiscard]] bool same(
    const std::uint8_t* const first, const std::uint8_t* const second, const std::size_t size) {
    for (std::size_t index = 0; index < size; ++index) {
        if (first[index] != second[index]) {
            return false;
        }
    }
    return true;
}

} // namespace

NetParamResult read_net_param_record(const std::uint8_t* const sector,
    const std::size_t sector_size, const std::uint32_t record_type, void* const data,
    const std::size_t capacity, std::size_t& size) {
    size = 0U;
    TableInfo info{};
    if (!inspect(sector, sector_size, record_type, info)) {
        return NetParamResult::invalid;
    }
    if (!info.table_present || !info.record_present) {
        return NetParamResult::not_found;
    }
    const auto payload_size = info.record_size - kHeaderSize;
    if (data == nullptr || payload_size > capacity) {
        return NetParamResult::no_space;
    }
    std::memcpy(data, sector + info.record_offset + kHeaderSize, payload_size);
    size = payload_size;
    return NetParamResult::succeeded;
}

NetParamResult replace_net_param_record(const std::uint8_t* const source,
    std::uint8_t* const destination, const std::size_t sector_size, const std::uint32_t record_type,
    const void* const data, const std::size_t size) {
    if (source == nullptr || destination == nullptr || data == nullptr || size == 0U ||
        size > std::numeric_limits<std::uint32_t>::max()) {
        return NetParamResult::invalid;
    }
    TableInfo info{};
    if (!inspect(source, sector_size, record_type, info)) {
        return NetParamResult::invalid;
    }

    const std::size_t old_end = info.table_present ? info.end : kHeaderSize;
    const std::size_t old_record_size = info.record_present ? info.record_size : 0U;
    if (size > sector_size - 2U * kHeaderSize) {
        return NetParamResult::no_space;
    }
    const std::size_t new_record_size = kHeaderSize + size;
    const std::size_t retained_size = old_end - old_record_size;
    if (new_record_size > sector_size - retained_size) {
        return NetParamResult::no_space;
    }
    const std::size_t new_end = retained_size + new_record_size;
    if (new_end > old_end && !erased(source + old_end, new_end - old_end)) {
        return NetParamResult::no_space;
    }

    std::memcpy(destination, source, sector_size);
    if (!info.table_present) {
        std::memset(destination, 0xff, sector_size);
    }
    write_u32(destination, kNetParamTableType);
    write_u32(destination + 4U, static_cast<std::uint32_t>(new_end - kHeaderSize));

    const std::size_t insert = info.record_present ? info.record_offset : old_end;
    const std::size_t suffix = insert + old_record_size;
    const std::size_t suffix_size = old_end - suffix;
    write_u32(destination + insert, record_type);
    write_u32(destination + insert + 4U, static_cast<std::uint32_t>(size));
    std::memcpy(destination + insert + kHeaderSize, data, size);
    if (suffix_size != 0U) {
        std::memcpy(destination + insert + new_record_size, source + suffix, suffix_size);
    }
    if (new_end < old_end) {
        std::memset(destination + new_end, 0xff, old_end - new_end);
    }
    return same(source, destination, sector_size) ? NetParamResult::unchanged
                                                  : NetParamResult::succeeded;
}

NetParamResult remove_net_param_record(const std::uint8_t* const source,
    std::uint8_t* const destination, const std::size_t sector_size,
    const std::uint32_t record_type) {
    if (source == nullptr || destination == nullptr) {
        return NetParamResult::invalid;
    }
    TableInfo info{};
    if (!inspect(source, sector_size, record_type, info)) {
        return NetParamResult::invalid;
    }
    if (!info.table_present || !info.record_present) {
        return NetParamResult::not_found;
    }

    std::memcpy(destination, source, sector_size);
    const std::size_t suffix = info.record_offset + info.record_size;
    const std::size_t suffix_size = info.end - suffix;
    if (suffix_size != 0U) {
        std::memcpy(destination + info.record_offset, source + suffix, suffix_size);
    }
    const std::size_t new_end = info.end - info.record_size;
    std::memset(destination + new_end, 0xff, info.end - new_end);
    write_u32(destination + 4U, static_cast<std::uint32_t>(new_end - kHeaderSize));
    return NetParamResult::succeeded;
}

} // namespace newmota::platform::beken
