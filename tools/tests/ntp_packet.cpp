/*
  ntp_packet.cpp - host safety checks for served UTC packets

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ntp_packet.cpp
 * @brief Protects downstream persistent RTC consumers from malformed or false-quality UTC.
 */
#include "newmota/platform/ntp_packet.h"
#include "services/time/clock_elapsed.h"
#include <cstdio>
#include <cstring>

namespace {
using namespace newmota::platform;
using namespace newmota::platform::ntp_packet;
int failures{};

void expect(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "NTP packet: %s\n", message);
        ++failures;
    }
}

std::uint32_t read32(const Packet& bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) | bytes[offset + 3];
}

NtpServerClock gps_clock() {
    NtpServerClock clock{};
    clock.monotonic_ms = UINT32_MAX - 99U;
    clock.utc_seconds = 1'700'000'000;
    clock.millisecond = 750;
    clock.reference_seconds = 1'699'999'999;
    clock.reference_millisecond = 250;
    clock.age_ms = 1500;
    clock.maximum_age_ms = 5000;
    clock.root_dispersion = 65536;
    clock.stratum = 1;
    clock.leap = 0;
    clock.reference_id = {'G', 'P', 'S', 0};
    clock.quality = NtpServerQuality::gps;
    return clock;
}
} // namespace

int main() {
    expect(newmota::time_clock::elapsed(999, 1000) == 0 &&
               newmota::time_clock::elapsed(UINT32_MAX, 0) == 0,
        "cached timestamps before a new anchor never derive 49-day UTC jumps");
    expect(newmota::time_clock::elapsed(100, UINT32_MAX - 99U) == 200 &&
               newmota::time_clock::elapsed(2'500'000'000U, 0) == 2'500'000'000U,
        "genuine forward monotonic wrap and long intervals remain representable");
    expect(newmota::time_clock::elapsed(0, 60'000) == 0 &&
               newmota::time_clock::elapsed(0, 60'001) != 0,
        "stale-caller tolerance is bounded and explicit");
    Packet input{};
    input[0] = 0x23;
    input[2] = 6;
    expect(request(input.data(), input.size()),
        "v4 mode3 accepts an unsynchronized client's zero token");
    expect(!request(nullptr, input.size()), "null input rejected");
    for (std::size_t size = 0; size < 80; ++size)
        expect(request(input.data(), size) == (size == 48), "only exact base header accepted");
    for (unsigned byte = 0; byte < 256; ++byte) {
        input[0] = static_cast<std::uint8_t>(byte);
        const auto version = (byte >> 3U) & 7U;
        expect(request(input.data(), input.size()) ==
                   ((version == 3 || version == 4) && (byte & 7U) == 3),
            "only v3/v4 client mode accepted; request leap bits ignored");
    }
    input[0] = 0x1b;
    for (std::size_t i = 40; i < input.size(); ++i)
        input[i] = static_cast<std::uint8_t>(i * 5U);
    auto clock = gps_clock();
    auto output = reply(input, clock, 50, 450);
    expect(output[0] == 0x1c && output[1] == 1 && output[2] == 6 && output[3] == 0xf7,
        "reply echoes version/poll and advertises millisecond resolution");
    expect(std::memcmp(output.data() + 24, input.data() + 40, 8) == 0,
        "correlation token copied byte for byte");
    expect(read32(output, 16) == clock.reference_seconds + 2'208'988'800U &&
               read32(output, 20) == 0x40000000U,
        "original sample reference timestamp remains independent of current clock");
    expect(read32(output, 32) == clock.utc_seconds + 2'208'988'800U &&
               read32(output, 36) == 3'865'470'566U,
        "native arrival uses wrapping monotonic anchor and 900ms fraction");
    expect(read32(output, 40) == clock.utc_seconds + 1U + 2'208'988'800U &&
               read32(output, 44) == 1'288'490'188U,
        "native departure carries a second separately from arrival");
    expect(read32(output, 8) >= 65536U && read32(output, 12) == 0x47505300U,
        "GPS provenance and one-second policy allowance encoded");

    output = reply(input, clock, 3399, 3400);
    expect(output[0] >> 6U == 3 && output[1] == 16,
        "automatic source expires exactly at its age limit across monotonic wrap");
    for (std::size_t i = 4; i < output.size(); ++i)
        if (i < 24 || i >= 32)
            expect(output[i] == 0, "unsynchronized time and reference fields stay zero");
    expect(std::memcmp(output.data() + 24, input.data() + 40, 8) == 0,
        "expired source still correlates its reply");
    clock = {};
    output = reply(input, clock, 0, 0);
    expect(output[1] == 16 && output[0] >> 6U == 3,
        "never-synchronized server cannot advertise valid UTC");

    clock = gps_clock();
    clock.monotonic_ms = 0;
    clock.age_ms = 0;
    clock.quality = NtpServerQuality::ntp;
    clock.stratum = 4;
    clock.leap = 1;
    clock.reference_id = {192, 0, 2, 12};
    clock.root_delay = 32768;
    clock.root_dispersion = 65536 + 8192;
    clock.maximum_age_ms = 3'605'000;
    output = reply(input, clock, 3'599'999, 3'600'000);
    expect(output[1] == 4 && output[0] >> 6U == 1 && read32(output, 12) == 0xc000020cU &&
               read32(output, 4) == 32768U,
        "relay preserves upstream-derived stratum/leap/IPv4 reference/delay");
    expect(read32(output, 8) == 77'266U,
        "relay adds upstream dispersion, one second allowance and nominal15ppm hourly aging");
    expect(quality(clock, 3'604'999) == NtpServerQuality::ntp &&
               quality(clock, 3'605'000) == NtpServerQuality::holdover,
        "NTP has one refresh interval plus deadline before unsynchronized holdover");
    clock.stratum = 16;
    expect(quality(clock, 0) == NtpServerQuality::holdover,
        "upstream stratum15 cannot become a synchronized stratum16 relay");

    clock = gps_clock();
    clock.monotonic_ms = 0;
    clock.quality = NtpServerQuality::local;
    clock.stratum = 10;
    clock.maximum_age_ms = UINT32_MAX;
    clock.reference_id = {127, 127, 1, 0};
    clock.age_ms = 2ULL * UINT32_MAX;
    output = reply(input, clock, 1, 2);
    expect(output[1] == 10 && output[0] >> 6U == 0 && read32(output, 12) == 0x7f7f0100U,
        "explicit local fallback stays distinct from GPS after multiple monotonic wraps");
    expect(read32(output, 8) > 100U * 65536U, "long local hold accumulates uncertainty");
    clock.age_ms = static_cast<std::uint64_t>(UINT32_MAX) * 1000U;
    clock.root_dispersion = UINT32_MAX;
    output = reply(input, clock, 1, 2);
    expect(read32(output, 8) == UINT32_MAX, "large dispersion saturates without wrap");

    clock = gps_clock();
    clock.monotonic_ms = 0;
    clock.utc_seconds = 2'085'978'496U; // 2036-02-07 06:28:16, the first NTP era rollover.
    clock.millisecond = 0;
    output = reply(input, clock, 500, 501);
    expect(read32(output, 32) == 0 && read32(output, 36) == 0x80000000U,
        "2036 NTP era rollover retains nonzero fractions");
    clock.utc_seconds = UINT32_MAX - 1U;
    output = reply(input, clock, 1500, 2000);
    expect(output[0] >> 6U == 3 && output[1] == 16,
        "exhausted Unix clock cannot silently wrap into a trusted downstream epoch");
    return failures == 0 ? 0 : 1;
}
