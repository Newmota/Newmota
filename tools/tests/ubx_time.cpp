/*
  ubx_time.cpp - host checks for trusted UBX time intake

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ubx_time.cpp
 * @brief Rejects malformed receiver UTC before it can program a persistent RTC.
 */
#include "drivers/gps/ubx_protocol.h"
#include "support/utc_calendar.h"
#include <array>
#include <cstdio>

namespace {
using namespace newmota::gps::ubx;
int failures{};

void expect(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "UBX time: %s\n", message);
        ++failures;
    }
}

void put32(Frame& frame, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        frame.payload[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

Frame utc_frame() {
    Frame frame{};
    frame.message_class = 1;
    frame.id = 0x21;
    frame.length = 20;
    put32(frame, 0, 1000);
    frame.payload[12] = 0xe8;
    frame.payload[13] = 7; // 2024
    frame.payload[14] = 2;
    frame.payload[15] = 29;
    frame.payload[16] = 12;
    frame.payload[19] = 7;
    return frame;
}

std::size_t feed(
    Parser& parser, const std::uint8_t* bytes, std::size_t size, std::uint32_t now = 0) {
    std::size_t frames{};
    for (std::size_t i = 0; i < size; ++i)
        frames += parser.feed(bytes[i], now);
    return frames;
}
} // namespace

int main() {
    std::uint32_t calendar{};
    expect(newmota::utc_calendar::epoch(2106, 2, 7, 6, 28, 15, calendar) && calendar == UINT32_MAX,
        "shared calendar accepts the final uint32 epoch second");
    expect(!newmota::utc_calendar::epoch(2106, 2, 7, 6, 28, 16, calendar),
        "shared calendar rejects epoch overflow");
    expect(!newmota::utc_calendar::epoch(2100, 2, 29, 0, 0, 0, calendar),
        "shared calendar rejects the non-leap century day");
    Frame frame = utc_frame();
    Utc utc{};
    expect(decode_utc(frame, utc) && utc.seconds == 1709208000U && !utc.millisecond,
        "leap-day UTC uses the expected Unix epoch");
    const auto epoch = utc.seconds;
    for (std::uint8_t bits = 0; bits < 7; ++bits) {
        frame.payload[19] = bits;
        expect(!decode_utc(frame, utc), "each incomplete time validity combination is rejected");
    }
    frame = utc_frame();
    put32(frame, 8, static_cast<std::uint32_t>(-1000000));
    expect(decode_utc(frame, utc) && utc.seconds == epoch - 1 && utc.millisecond == 999,
        "negative nanoseconds borrow a second without unsigned wrap");
    put32(frame, 8, 1000000000U);
    expect(decode_utc(frame, utc) && utc.seconds == epoch + 1 && !utc.millisecond,
        "positive one-second fraction carries into epoch");
    put32(frame, 8, static_cast<std::uint32_t>(-1000000000));
    expect(decode_utc(frame, utc) && utc.seconds == epoch - 1 && !utc.millisecond,
        "negative one-second fraction is representable");
    put32(frame, 8, 1000000001U);
    expect(!decode_utc(frame, utc), "out-of-range nanoseconds are rejected");
    frame = utc_frame();
    frame.payload[18] = 60;
    expect(!decode_utc(frame, utc), "unrepresentable leap second is skipped");
    frame = utc_frame();
    frame.payload[12] = 0xe7;
    expect(!decode_utc(frame, utc), "non-leap February 29 is rejected");
    frame = utc_frame();
    frame.payload[14] = 0;
    expect(!decode_utc(frame, utc), "zero month is rejected before calendar indexing");
    frame = utc_frame();
    frame.payload[15] = 30;
    expect(!decode_utc(frame, utc), "February 30 is rejected");
    frame = utc_frame();
    frame.payload[16] = 24;
    expect(!decode_utc(frame, utc), "hour 24 is rejected");
    frame = utc_frame();
    frame.payload[17] = 60;
    expect(!decode_utc(frame, utc), "minute 60 is rejected");
    frame = utc_frame();
    frame.payload[12] = 0xdf;
    expect(!decode_utc(frame, utc), "untrusted old epoch is rejected");
    frame = utc_frame();
    put32(frame, 0, 604800000U);
    expect(!decode_utc(frame, utc), "out-of-week iTOW is rejected");

    frame = utc_frame();
    // Independent wire checksum and payload, rather than relying on the encoder under test.
    std::array<std::uint8_t, 28> wire{0xb5, 0x62, 1, 0x21, 20, 0};
    for (std::size_t i = 0; i < 20; ++i)
        wire[6 + i] = frame.payload[i];
    unsigned a{}, b{};
    for (std::size_t i = 2; i < 26; ++i) {
        a = (a + wire[i]) & 255U;
        b = (b + a) & 255U;
    }
    wire[26] = static_cast<std::uint8_t>(a);
    wire[27] = static_cast<std::uint8_t>(b);
    Parser parser;
    expect(feed(parser, wire.data(), wire.size()) == 1 && decode_utc(parser.frame(), utc) &&
               utc.seconds == epoch,
        "bytewise checksummed frame reaches trusted UTC intake");
    wire[27] ^= 1;
    expect(feed(parser, wire.data(), wire.size()) == 0 && parser.errors().checksum == 1,
        "corrupt checksum never reaches time intake");
    wire[27] ^= 1;
    expect(feed(parser, wire.data(), 10, 0xfffffff0U) == 0, "partial frame is withheld");
    parser.expire(0x600U);
    expect(parser.errors().timeout == 1 && feed(parser, wire.data(), wire.size(), 0x601U) == 1,
        "truncated frame expires across monotonic wrap and next frame recovers");
    std::array<std::uint8_t, 128> other{};
    auto size = encode(0x0a, 0x77, wire.data(), wire.size(), other.data(), other.size());
    expect(size && feed(parser, other.data(), size, 0x602U) == 0,
        "valid unknown frame does not expose embedded time frame");
    size = encode(1, 0x21, frame.payload.data(), 19, other.data(), other.size());
    expect(size && feed(parser, other.data(), size, 0x603U) == 0 && parser.errors().length == 1,
        "short known message is rejected even with a valid checksum");
    expect(feed(parser, wire.data(), wire.size(), 0x604U) == 1,
        "valid frame follows unknown and wrong-length frames");
    expect(encode(1, 0x21, nullptr, 1, other.data(), other.size()) == 0,
        "encoder rejects a missing payload");
    expect(encode(1, 0x21, frame.payload.data(), 20, other.data(), 27) == 0,
        "encoder enforces destination capacity");
    return failures ? 1 : 0;
}
