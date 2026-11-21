/*
  ubx_protocol.h - Bounded u-blox binary protocol

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ubx_protocol.h
 * @brief Fixed-storage UBX framing and checked navigation decoding.
 */
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace newmota::gps::ubx {
/** @brief A checked UBX frame; only recognized payloads are retained. */
struct Frame {
    std::uint8_t message_class{}, id{};
    std::uint16_t length{};
    std::array<std::uint8_t, 36> payload{};
};

/** @brief Cumulative framing failures, retained across parser resets. */
struct Errors {
    std::uint32_t checksum{}, length{}, timeout{};
};

/** @brief Streaming parser; unknown frames are consumed without interpreting embedded sync. */
class Parser final {
  public:
    /** @brief Drops a partial frame without clearing diagnostics. */
    void reset();
    /** @brief Expires a partial frame after 1500 ms, including a wrapping clock. */
    void expire(std::uint32_t now);
    /** @brief Consumes one byte; true exposes a recognized exact-length checked frame. */
    bool feed(std::uint8_t byte, std::uint32_t now);

    [[nodiscard]] const Frame& frame() const { return frame_; }

    [[nodiscard]] Errors errors() const { return errors_; }

  private:
    Frame frame_{};
    Errors errors_{};
    std::uint32_t started_{};
    std::uint16_t offset_{};
    std::uint8_t state_{}, a_{}, b_{}, received_a_{};
    bool retain_{};
};

/** @brief UTC normalized to a nonnegative millisecond fraction; iTOW is GPS-week ms. */
struct Utc {
    std::uint32_t itow{}, seconds{};
    std::uint16_t millisecond{};
    std::uint32_t accuracy_ns{};
};

struct Fix {
    std::uint32_t itow{};
    std::uint8_t type{};
    bool valid{};
};

/** @brief Position uses degrees * 1e7 and millimetres, without presentation rounding. */
struct Position {
    std::uint32_t itow{}, horizontal_accuracy{}, vertical_accuracy{};
    std::int32_t longitude{}, latitude{}, altitude{}, altitude_msl{};
};

/** @brief Velocity uses cm/s and heading degrees * 1e5. */
struct Velocity {
    std::uint32_t itow{}, speed{}, ground_speed{}, speed_accuracy{}, heading_accuracy{};
    std::int32_t north{}, east{}, down{}, heading{};
};

/** @brief Decodes only valid Gregorian UTC with all three validity bits and trusted epoch. */
[[nodiscard]] bool decode_utc(const Frame&, Utc&);
[[nodiscard]] bool decode_fix(const Frame&, Fix&);
[[nodiscard]] bool decode_position(const Frame&, Position&);
[[nodiscard]] bool decode_velocity(const Frame&, Velocity&);
/** @brief Encodes one bounded UBX command, returning zero for invalid buffers. */
std::size_t encode(std::uint8_t message_class, std::uint8_t id, const std::uint8_t* payload,
    std::size_t size, std::uint8_t* output, std::size_t capacity);
} // namespace newmota::gps::ubx
