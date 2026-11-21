/*
  matter_wire.h - bounded Matter frame, exchange, and TLV codec

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace newmota::matter {

inline constexpr std::size_t kMaxFrameSize = 1232;
inline constexpr std::size_t kMicSize = 16;
inline constexpr std::size_t kNodeIdSize = 8;

/** @brief Unencrypted message header, which is also the AES-CCM additional data. */
struct FrameHeader {
    std::uint16_t session_id{};
    std::uint8_t security_flags{};
    std::uint32_t counter{};
    bool has_source{};
    bool has_destination{};
    std::uint8_t source[kNodeIdSize]{};
    std::uint8_t destination[kNodeIdSize]{};
    std::size_t size{};
};

/** @brief Parses a version 0 unicast header; privacy, control and group frames are refused. */
[[nodiscard]] bool parse_frame(const std::uint8_t* data, std::size_t size, FrameHeader& header);
/** @brief Writes a header and sets its size; 0 when it does not fit. */
std::size_t write_frame(std::uint8_t* output, std::size_t capacity, FrameHeader& header);

inline constexpr std::uint8_t kExchangeInitiator = 0x01;
inline constexpr std::uint8_t kExchangeAck = 0x02;
inline constexpr std::uint8_t kExchangeReliable = 0x04;
inline constexpr std::uint8_t kExchangeSecuredExtensions = 0x08;
inline constexpr std::uint8_t kExchangeVendor = 0x10;

/** @brief Exchange layer header, encrypted on secure sessions. */
struct ProtocolHeader {
    std::uint8_t flags{};
    std::uint8_t opcode{};
    std::uint16_t exchange{};
    std::uint16_t protocol{};
    std::uint32_t ack_counter{};
    std::size_t size{};
};

/** @brief Parses the header and skips any secured extensions inside the buffer. */
[[nodiscard]] bool parse_protocol(
    const std::uint8_t* data, std::size_t size, ProtocolHeader& header);
/** @brief Writes a standard-protocol header and sets its size; 0 when it does not fit. */
std::size_t write_protocol(std::uint8_t* output, std::size_t capacity, ProtocolHeader& header);

enum class TlvType : std::uint8_t {
    signed_integer,
    unsigned_integer,
    boolean,
    real,
    text,
    octets,
    null,
    structure,
    array,
    list,
    end,
};

/** @brief One TLV element; text and octets borrow from the parsed buffer. */
struct TlvElement {
    TlvType type{TlvType::null};
    bool has_tag{};
    std::uint32_t tag{};
    std::uint8_t width{};
    std::uint64_t value{};
    double real{};
    const std::uint8_t* data{};
    std::size_t length{};
    std::size_t next{};
};

/** @brief Decodes the element at offset, checking every length against size. */
[[nodiscard]] bool tlv_next(
    const std::uint8_t* data, std::size_t size, std::size_t offset, TlvElement& element);

/** @brief Moves offset past an element and everything inside it, within a nesting limit. */
[[nodiscard]] bool tlv_skip(
    const std::uint8_t* data, std::size_t size, std::size_t& offset, const TlvElement& element);

/**
 * @brief Copies an anonymous element, giving it a context tag.
 * @return The new size, or 0 when it already has a tag or does not fit.
 */
[[nodiscard]] std::size_t tlv_with_context_tag(const std::uint8_t* element, std::size_t size,
    std::uint8_t tag, std::uint8_t* output, std::size_t capacity);

} // namespace newmota::matter
