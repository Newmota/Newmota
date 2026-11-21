/*
  ntp_packet.cpp - pure bounded NTP server packet encoding

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ntp_packet.cpp
 * @brief Independent RFC5905 header encoding without SDK or packed-struct assumptions.
 */
#include "newmota/platform/ntp_packet.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace newmota::platform::ntp_packet {
namespace {
void write32(std::uint8_t* out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        out[i] = static_cast<std::uint8_t>(value >> (24U - 8U * i));
}

/** @brief Encodes modular NTP seconds (including the 2036 era) and millisecond fractions. */
void timestamp(std::uint8_t* out, std::uint32_t seconds, std::uint64_t milliseconds) {
    const auto increment = milliseconds / 1000U;
    // UTC exhaustion is not another NTP era: never silently wrap a uint32 Unix clock.
    seconds = increment > std::numeric_limits<std::uint32_t>::max() - seconds
                  ? std::numeric_limits<std::uint32_t>::max()
                  : seconds + static_cast<std::uint32_t>(increment);
    write32(out, seconds + 2'208'988'800U);
    write32(out + 4, static_cast<std::uint32_t>(((milliseconds % 1000U) << 32U) / 1000U));
}
} // namespace

bool request(const std::uint8_t* bytes, std::size_t size) {
    if (bytes == nullptr || size != Packet{}.size())
        return false;
    const auto version = (bytes[0] >> 3U) & 7U;
    return (version == 3 || version == 4) && (bytes[0] & 7U) == 3;
}

NtpServerQuality quality(const NtpServerClock& clock, std::uint32_t now) {
    if (clock.quality == NtpServerQuality::none)
        return NtpServerQuality::none;
    const auto age = static_cast<std::uint64_t>(clock.age_ms) +
                     static_cast<std::uint32_t>(now - clock.monotonic_ms);
    const auto advance = (static_cast<std::uint64_t>(clock.millisecond) +
                             static_cast<std::uint32_t>(now - clock.monotonic_ms)) /
                         1000U;
    if (clock.stratum >= 16 || clock.leap == 3 || advance >= UINT32_MAX - clock.utc_seconds ||
        (clock.maximum_age_ms != UINT32_MAX && age >= clock.maximum_age_ms))
        return NtpServerQuality::holdover;
    return clock.quality;
}

const char* quality_name(NtpServerQuality value) {
    switch (value) {
    case NtpServerQuality::gps:
        return "gps";
    case NtpServerQuality::ntp:
        return "ntp";
    case NtpServerQuality::local:
        return "local";
    case NtpServerQuality::holdover:
        return "holdover";
    case NtpServerQuality::none:
        return "none";
    }
    return "none";
}

Packet reply(const Packet& input, const NtpServerClock& clock, std::uint32_t received_ms,
    std::uint32_t sent_ms) {
    Packet out{};
    const auto state = quality(clock, sent_ms);
    const bool valid = state != NtpServerQuality::none && state != NtpServerQuality::holdover;
    out[0] = static_cast<std::uint8_t>((valid ? clock.leap : 3U) << 6U | (input[0] & 0x38U) | 4U);
    out[1] = valid ? clock.stratum : 16;
    out[2] = input[2];
    out[3] = 0xf7; // -9: approximately 2 ms clock-reading resolution, not UTC accuracy.
    std::memcpy(out.data() + 24, input.data() + 40, 8);
    if (!valid)
        return out;
    write32(out.data() + 4, clock.root_delay);
    const auto age_ms = static_cast<std::uint64_t>(clock.age_ms) +
                        static_cast<std::uint32_t>(sent_ms - clock.monotonic_ms);
    // RFC5905's nominal 15 ppm model is a policy assumption, not characterized accuracy.
    const auto dispersion = static_cast<std::uint64_t>(clock.root_dispersion) +
                            (age_ms * 15U * 65536U) / 1'000'000'000U;
    write32(out.data() + 8,
        static_cast<std::uint32_t>(std::min<std::uint64_t>(dispersion, UINT32_MAX)));
    std::memcpy(out.data() + 12, clock.reference_id.data(), clock.reference_id.size());
    timestamp(out.data() + 16, clock.reference_seconds, clock.reference_millisecond);
    timestamp(out.data() + 32, clock.utc_seconds,
        static_cast<std::uint64_t>(clock.millisecond) +
            static_cast<std::uint32_t>(received_ms - clock.monotonic_ms));
    timestamp(out.data() + 40, clock.utc_seconds,
        static_cast<std::uint64_t>(clock.millisecond) +
            static_cast<std::uint32_t>(sent_ms - clock.monotonic_ms));
    return out;
}
} // namespace newmota::platform::ntp_packet

namespace newmota::platform {
NtpServerQuality ntp_server_quality(const NtpServerClock& clock, std::uint32_t now) {
    return ntp_packet::quality(clock, now);
}

const char* ntp_server_quality_name(NtpServerQuality value) {
    return ntp_packet::quality_name(value);
}
} // namespace newmota::platform
