/*
  ubx_protocol.cpp - Bounded u-blox binary protocol

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ubx_protocol.cpp
 * @brief Explicit little-endian UBX fields, checksums and Gregorian UTC conversion.
 */
#include "drivers/gps/ubx_protocol.h"
#include "support/utc_calendar.h"

namespace newmota::gps::ubx {
namespace {
std::uint16_t u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (std::uint16_t(p[1]) << 8));
}

std::uint32_t u32(const std::uint8_t* p) {
    return p[0] | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
           (std::uint32_t(p[3]) << 24);
}

std::int32_t i32(const std::uint8_t* p) {
    const auto value = u32(p);
    return value <= INT32_MAX ? static_cast<std::int32_t>(value)
                              : -1 - static_cast<std::int32_t>(UINT32_MAX - value);
}

unsigned expected(std::uint8_t cls, std::uint8_t id) {
    if (cls == 5 && (id == 0 || id == 1))
        return 2;
    if (cls != 1)
        return 0;
    switch (id) {
    case 0x21:
        return 20;
    case 3:
        return 16;
    case 2:
        return 28;
    case 0x12:
        return 36;
    default:
        return 0;
    }
}

bool matches(const Frame& f, std::uint8_t id, unsigned length) {
    return f.message_class == 1 && f.id == id && f.length == length &&
           u32(f.payload.data()) < 604800000U;
}
} // namespace

void Parser::reset() {
    state_ = 0;
    offset_ = 0;
    a_ = b_ = 0;
    retain_ = false;
}

void Parser::expire(std::uint32_t now) {
    if (state_ && now - started_ >= 1500U) {
        ++errors_.timeout;
        reset();
    }
}

bool Parser::feed(std::uint8_t byte, std::uint32_t now) {
    expire(now);
    if (state_ == 0) {
        if (byte == 0xb5) {
            state_ = 1;
            started_ = now;
        }
        return false;
    }
    if (state_ == 1) {
        if (byte == 0x62)
            state_ = 2;
        else if (byte != 0xb5)
            reset();
        return false;
    }
    if (state_ <= 6) {
        a_ = static_cast<std::uint8_t>(a_ + byte);
        b_ = static_cast<std::uint8_t>(b_ + a_);
    }
    switch (state_) {
    case 2:
        frame_.message_class = byte;
        state_ = 3;
        break;
    case 3:
        frame_.id = byte;
        state_ = 4;
        break;
    case 4:
        frame_.length = byte;
        state_ = 5;
        break;
    case 5: {
        frame_.length |= std::uint16_t(byte) << 8;
        const auto size = expected(frame_.message_class, frame_.id);
        retain_ = size && size == frame_.length;
        if (size && !retain_)
            ++errors_.length;
        state_ = frame_.length ? 6 : 7;
        break;
    }
    case 6:
        if (retain_)
            frame_.payload[offset_] = byte;
        if (++offset_ == frame_.length)
            state_ = 7;
        break;
    case 7:
        received_a_ = byte;
        state_ = 8;
        break;
    case 8: {
        const bool checksum = received_a_ == a_ && byte == b_;
        const bool ready = checksum && retain_;
        if (!checksum)
            ++errors_.checksum;
        reset();
        return ready;
    }
    default:
        reset();
        break;
    }
    return false;
}

bool decode_utc(const Frame& f, Utc& out) {
    if (!matches(f, 0x21, 20))
        return false;
    const auto* p = f.payload.data();
    if ((p[19] & 7) != 7)
        return false;
    const auto year = u16(p + 12);
    const unsigned month = p[14], day = p[15], hour = p[16], minute = p[17], second = p[18];
    std::uint32_t base{};
    if (year > 2099 || !utc_calendar::epoch(year, month, day, hour, minute, second, base))
        return false;
    std::int64_t seconds = base;
    auto nano = i32(p + 8);
    if (nano < -1000000000 || nano > 1000000000)
        return false;
    if (nano < 0) {
        --seconds;
        nano += 1000000000;
    }
    if (nano == 1000000000) {
        ++seconds;
        nano = 0;
    }
    if (seconds < utc_calendar::kFirstValidTime || seconds > UINT32_MAX)
        return false;
    out = {u32(p), static_cast<std::uint32_t>(seconds), static_cast<std::uint16_t>(nano / 1000000),
        u32(p + 4)};
    return true;
}

bool decode_fix(const Frame& f, Fix& out) {
    if (!matches(f, 3, 16))
        return false;
    const auto* p = f.payload.data();
    out = {u32(p), p[4], bool((p[5] & 1) && (p[4] == 2 || p[4] == 3 || p[4] == 4))};
    return true;
}

bool decode_position(const Frame& f, Position& out) {
    if (!matches(f, 2, 28))
        return false;
    const auto* p = f.payload.data();
    const auto lon = i32(p + 4), lat = i32(p + 8);
    if (lon < -1800000000 || lon > 1800000000 || lat < -900000000 || lat > 900000000)
        return false;
    out = {u32(p), u32(p + 20), u32(p + 24), lon, lat, i32(p + 12), i32(p + 16)};
    return true;
}

bool decode_velocity(const Frame& f, Velocity& out) {
    if (!matches(f, 0x12, 36))
        return false;
    const auto* p = f.payload.data();
    const auto heading = i32(p + 24);
    if (heading < 0 || heading > 36000000)
        return false;
    out = {u32(p), u32(p + 16), u32(p + 20), u32(p + 28), u32(p + 32), i32(p + 4), i32(p + 8),
        i32(p + 12), heading};
    return true;
}

std::size_t encode(std::uint8_t cls, std::uint8_t id, const std::uint8_t* payload, std::size_t size,
    std::uint8_t* output, std::size_t capacity) {
    if (size > UINT16_MAX || !output || capacity < size + 8 || (size && !payload))
        return 0;
    output[0] = 0xb5;
    output[1] = 0x62;
    output[2] = cls;
    output[3] = id;
    output[4] = static_cast<std::uint8_t>(size);
    output[5] = static_cast<std::uint8_t>(size >> 8);
    for (std::size_t i = 0; i < size; ++i)
        output[6 + i] = payload[i];
    std::uint8_t a{}, b{};
    for (std::size_t i = 2; i < size + 6; ++i) {
        a = static_cast<std::uint8_t>(a + output[i]);
        b = static_cast<std::uint8_t>(b + a);
    }
    output[size + 6] = a;
    output[size + 7] = b;
    return size + 8;
}
} // namespace newmota::gps::ubx
