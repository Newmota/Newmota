/*
  matter_wire.cpp - bounded Matter frame, exchange, and TLV codec

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/matter/matter_wire.h"

#include <cstring>

namespace newmota::matter {
namespace {
constexpr std::uint8_t kFlagSource = 0x04;
constexpr std::uint8_t kFlagDestinationMask = 0x03;
constexpr std::uint8_t kSecurityRefused = 0xC0; // Privacy and control messages.
constexpr std::uint8_t kSecuritySessionType = 0x03;
constexpr std::uint8_t kSecurityExtensions = 0x20;
constexpr std::uint8_t kFlagExtensionsVersionMask = 0xF0;

/** @brief Reads a little-endian value of up to eight bytes. */
[[nodiscard]] std::uint64_t read_le(const std::uint8_t* data, const std::size_t size) {
    std::uint64_t value = 0;
    for (std::size_t index = size; index > 0; --index)
        value = (value << 8U) | data[index - 1];
    return value;
}

/** @brief Writes a little-endian value of up to four bytes. */
void write_le(std::uint8_t* data, std::uint32_t value, const std::size_t size) {
    for (std::size_t index = 0; index < size; ++index) {
        data[index] = static_cast<std::uint8_t>(value);
        value >>= 8U;
    }
}
} // namespace

bool parse_frame(const std::uint8_t* const data, const std::size_t size, FrameHeader& header) {
    header = {};
    if (size < 8U || (data[0] & kFlagExtensionsVersionMask) != 0U)
        return false;
    const std::uint8_t destination = data[0] & kFlagDestinationMask;
    header.session_id = static_cast<std::uint16_t>(read_le(data + 1, 2));
    header.security_flags = data[3];
    header.counter = static_cast<std::uint32_t>(read_le(data + 4, 4));
    // Unicast only: group destinations and group sessions wait for group transport.
    if (destination > 1U || (header.security_flags & kSecurityRefused) != 0U ||
        (header.security_flags & kSecuritySessionType) != 0U ||
        (header.security_flags & kSecurityExtensions) != 0U)
        return false;
    std::size_t offset = 8;
    if ((data[0] & kFlagSource) != 0U) {
        if (size < offset + kNodeIdSize)
            return false;
        header.has_source = true;
        std::memcpy(header.source, data + offset, kNodeIdSize);
        offset += kNodeIdSize;
    }
    if (destination == 1U) {
        if (size < offset + kNodeIdSize)
            return false;
        header.has_destination = true;
        std::memcpy(header.destination, data + offset, kNodeIdSize);
        offset += kNodeIdSize;
    }
    header.size = offset;
    return true;
}

std::size_t write_frame(
    std::uint8_t* const output, const std::size_t capacity, FrameHeader& header) {
    const std::size_t size =
        8U + (header.has_source ? kNodeIdSize : 0U) + (header.has_destination ? kNodeIdSize : 0U);
    if (capacity < size)
        return 0;
    output[0] = static_cast<std::uint8_t>(
        (header.has_source ? kFlagSource : 0U) | (header.has_destination ? 1U : 0U));
    write_le(output + 1, header.session_id, 2);
    output[3] = header.security_flags;
    write_le(output + 4, header.counter, 4);
    std::size_t offset = 8;
    if (header.has_source) {
        std::memcpy(output + offset, header.source, kNodeIdSize);
        offset += kNodeIdSize;
    }
    if (header.has_destination)
        std::memcpy(output + offset, header.destination, kNodeIdSize);
    header.size = size;
    return size;
}

bool parse_protocol(
    const std::uint8_t* const data, const std::size_t size, ProtocolHeader& header) {
    header = {};
    if (size < 6U)
        return false;
    header.flags = data[0];
    header.opcode = data[1];
    header.exchange = static_cast<std::uint16_t>(read_le(data + 2, 2));
    header.protocol = static_cast<std::uint16_t>(read_le(data + 4, 2));
    std::size_t offset = 6;
    if ((header.flags & kExchangeVendor) != 0U) {
        if (size < offset + 2U)
            return false;
        offset += 2;
    }
    if ((header.flags & kExchangeAck) != 0U) {
        if (size < offset + 4U)
            return false;
        header.ack_counter = static_cast<std::uint32_t>(read_le(data + offset, 4));
        offset += 4;
    }
    if ((header.flags & kExchangeSecuredExtensions) != 0U) {
        if (size < offset + 2U)
            return false;
        const std::size_t extension = static_cast<std::size_t>(read_le(data + offset, 2));
        offset += 2;
        if (extension > size - offset)
            return false;
        offset += extension;
    }
    header.size = offset;
    return true;
}

std::size_t write_protocol(
    std::uint8_t* const output, const std::size_t capacity, ProtocolHeader& header) {
    const bool ack = (header.flags & kExchangeAck) != 0U;
    const std::size_t size = 6U + (ack ? 4U : 0U);
    if (capacity < size)
        return 0;
    output[0] = header.flags;
    output[1] = header.opcode;
    write_le(output + 2, header.exchange, 2);
    write_le(output + 4, header.protocol, 2);
    if (ack)
        write_le(output + 6, header.ack_counter, 4);
    header.size = size;
    return size;
}

bool tlv_next(const std::uint8_t* const data, const std::size_t size, const std::size_t offset,
    TlvElement& element) {
    element = {};
    if (offset >= size)
        return false;
    const std::uint8_t control = data[offset];
    const std::uint8_t kind = control & 0x1FU;
    static constexpr std::uint8_t kTagSizes[8] = {0, 1, 2, 4, 2, 4, 6, 8};
    const std::size_t tag_size = kTagSizes[control >> 5U];
    std::size_t position = offset + 1U;
    if (tag_size > size - position)
        return false;
    element.has_tag = tag_size != 0U;
    // Keep the tag number; the vendor and profile of long forms are not used by Matter messages.
    if (tag_size != 0U) {
        const std::size_t number = tag_size == 6U || tag_size == 8U ? tag_size - 4U : 0U;
        element.tag =
            static_cast<std::uint32_t>(read_le(data + position + number, tag_size - number));
    }
    position += tag_size;

    std::size_t value_size = 0;
    if (kind <= 0x03U) {
        element.type = TlvType::signed_integer;
        value_size = std::size_t{1} << kind;
    } else if (kind <= 0x07U) {
        element.type = TlvType::unsigned_integer;
        value_size = std::size_t{1} << (kind - 4U);
    } else if (kind <= 0x09U) {
        element.type = TlvType::boolean;
        element.value = kind & 1U;
    } else if (kind <= 0x0BU) {
        element.type = TlvType::real;
        value_size = kind == 0x0AU ? 4U : 8U;
    } else if (kind <= 0x13U) {
        element.type = kind <= 0x0FU ? TlvType::text : TlvType::octets;
        const std::size_t length_size = std::size_t{1} << ((kind - 0x0CU) & 3U);
        if (length_size > size - position)
            return false;
        const std::uint64_t length = read_le(data + position, length_size);
        position += length_size;
        if (length > size - position)
            return false;
        element.data = data + position;
        element.length = static_cast<std::size_t>(length);
        element.next = position + element.length;
        return true;
    } else if (kind == 0x14U) {
        element.type = TlvType::null;
    } else if (kind == 0x15U) {
        element.type = TlvType::structure;
    } else if (kind == 0x16U) {
        element.type = TlvType::array;
    } else if (kind == 0x17U) {
        element.type = TlvType::list;
    } else if (kind == 0x18U) {
        element.type = TlvType::end;
    } else {
        return false;
    }
    if (value_size > size - position)
        return false;
    if (element.type == TlvType::real) {
        if (value_size == 4U) {
            float value = 0;
            std::memcpy(&value, data + position, 4);
            element.real = static_cast<double>(value);
        } else {
            std::memcpy(&element.real, data + position, 8);
        }
    } else if (value_size != 0U) {
        element.value = read_le(data + position, value_size);
        element.width = static_cast<std::uint8_t>(value_size);
    }
    element.next = position + value_size;
    return true;
}

bool tlv_skip(const std::uint8_t* const data, const std::size_t size, std::size_t& offset,
    const TlvElement& element) {
    offset = element.next;
    if (element.type != TlvType::structure && element.type != TlvType::array &&
        element.type != TlvType::list)
        return true;
    for (unsigned depth = 1; depth != 0U;) {
        TlvElement inner;
        if (depth > 8U || !tlv_next(data, size, offset, inner))
            return false;
        offset = inner.next;
        if (inner.type == TlvType::end)
            --depth;
        else if (inner.type == TlvType::structure || inner.type == TlvType::array ||
                 inner.type == TlvType::list)
            ++depth;
    }
    return true;
}

std::size_t tlv_with_context_tag(const std::uint8_t* const element, const std::size_t size,
    const std::uint8_t tag, std::uint8_t* const output, const std::size_t capacity) {
    if (size == 0U || (element[0] & 0xE0U) != 0U || capacity < size + 1U)
        return 0;
    output[0] = static_cast<std::uint8_t>(element[0] | 0x20U);
    output[1] = tag;
    std::memcpy(output + 2, element + 1, size - 1U);
    return size + 1U;
}

} // namespace newmota::matter
