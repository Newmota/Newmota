/*
  ntp_packet.h - pure bounded NTP server packet encoding

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ntp_packet.h
 * @brief SDK-independent NTPv3/v4 base-header validation and UTC response timestamps.
 */
#pragma once
#include "newmota/platform/ntp_server.h"
#include <cstddef>

namespace newmota::platform::ntp_packet {
using Packet = std::array<std::uint8_t, 48>;

/** @brief Accepts exactly the supported base header, including zero transmit tokens. */
[[nodiscard]] bool request(const std::uint8_t* bytes, std::size_t size);
/** @brief Evaluates automatic-source freshness at native processing time. */
[[nodiscard]] NtpServerQuality quality(const NtpServerClock& clock, std::uint32_t now);
/** @brief Returns stable compact status names. */
[[nodiscard]] const char* quality_name(NtpServerQuality quality);
/** @brief Builds a reply, echoing correlation bytes even when unsynchronized.
 * Arrival and departure use the same immutable clock publication; unsynchronized
 * replies carry LI3/stratum16 and zero time fields. Extension/MAC data is rejected.
 */
[[nodiscard]] Packet reply(const Packet& request, const NtpServerClock& clock,
    std::uint32_t received_ms, std::uint32_t sent_ms);
} // namespace newmota::platform::ntp_packet
