/*
  rtc_calendar.cpp - Host checks for persistent RTC calendar conversions

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file rtc_calendar.cpp
 * @brief Checks date encodings that could silently corrupt a battery-backed RTC calendar.
 */
#include "drivers/rtc/calendar_codec.h"
#include <cstdio>

namespace {
using namespace newmota::rtc::calendar;
int failures{};

void expect(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "RTC calendar: %s\n", message);
        ++failures;
    }
}
} // namespace

int main() {
    // 2024-02-29 12:34:56 UTC, Thursday. Register arrays are literal datasheet layouts.
    constexpr std::uint32_t leap_day = 1709210096U;
    constexpr Registers dallas{0x56, 0x34, 0x12, 5, 0x29, 0x02, 0x24};
    constexpr Registers nxp{0x56, 0x34, 0x12, 0x29, 4, 0x02, 0x24};
    constexpr Registers epson{0x56, 0x34, 0x12, 0x10, 0x29, 0x02, 0x24};
    constexpr Registers micro{0x56, 0x34, 0x12, 4, 0x29, 0x02, 0x24};
    const Format formats[]{kDallas, kNxp, kEpson, kMicroCrystal};
    const Registers arrays[]{dallas, nxp, epson, micro};
    std::uint32_t epoch{};
    for (unsigned i = 0; i < 4; ++i) {
        const auto format = formats[i];
        expect(decode(arrays[i], format, false, epoch) && epoch == leap_day,
            "each physical calendar layout decodes the golden leap-day timestamp");
        expect(encode(format, false, 2024, 2, 29, 12, 34, 56, 5) == arrays[i],
            "programming emits the golden weekday/day/month/year order");
        auto raw = arrays[i];
        raw[6] = 0x23;
        expect(!decode(raw, format, false, epoch), "non-leap February 29 is rejected");
        raw = arrays[i];
        raw[5] = 0;
        expect(!decode(raw, format, false, epoch), "month zero is rejected");
        raw = arrays[i];
        raw[5] |= 0x80;
        expect(!decode(raw, format, false, epoch),
            "unsupported century or reserved month flag is rejected");
        raw = arrays[i];
        raw[0] = 0x5a;
        expect(!decode(raw, format, false, epoch),
            "malformed BCD nibble cannot become a plausible time");
        raw = arrays[i];
        raw[0] = 0x60;
        expect(!decode(raw, format, false, epoch), "seconds overflow is rejected");
        raw = arrays[i];
        raw[6] = 0x15;
        expect(!decode(raw, format, false, epoch),
            "dates below the shared valid-clock threshold are rejected");
        raw = arrays[i];
        raw[format.weekday_first ? 3 : 4] = 0x80;
        expect(!decode(raw, format, false, epoch), "reserved weekday bits are rejected");
        raw = arrays[i];
        raw[format.weekday_first ? 4 : 3] = 0x30;
        expect(!decode(raw, format, false, epoch), "February 30 is rejected");
        raw = encode(format, false, 2099, 12, 31, 23, 59, 59, 5);
        expect(decode(raw, format, false, epoch) && epoch == 4102444799U,
            "last supported century second is representable without year-offset wrapping");
    }
    auto raw = epson;
    raw[3] = 0;
    expect(!decode(raw, kEpson, false, epoch), "zero Epson weekday is invalid");
    raw[3] = 3;
    expect(!decode(raw, kEpson, false, epoch), "multiple Epson weekday bits are invalid");
    expect(encode(kEpson, false, 2024, 3, 3, 0, 0, 0, 1)[3] == 1,
        "Epson Sunday uses bit zero without BCD transformation");
    expect(encode(kEpson, false, 2024, 3, 2, 0, 0, 0, 7)[3] == 0x40,
        "Epson Saturday uses bit six without BCD transformation");
    std::uint8_t hours{};
    expect(hour(0x12, true, hours) && hours == 0, "12 AM is midnight");
    expect(hour(0x32, true, hours) && hours == 12, "12 PM is noon");
    expect(hour(0x31, true, hours) && hours == 23, "11 PM is 23:00");
    expect(!hour(0, true, hours) && !hour(0x33, true, hours), "12-hour bounds are strict");
    expect(!hour(0x80, false, hours), "reserved hour bit cannot be silently masked");
    expect(encode(kNxp, true, 2024, 2, 29, 0, 0, 0, 5)[2] == 0x12 &&
               encode(kNxp, true, 2024, 2, 29, 12, 0, 0, 5)[2] == 0x32 &&
               encode(kNxp, true, 2024, 2, 29, 23, 0, 0, 5)[2] == 0x31,
        "writing preserves an external 12-hour mode with correct AM/PM encoding");
    return failures ? 1 : 0;
}
