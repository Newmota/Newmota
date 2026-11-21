/*
  ntp_server.h - bounded optional NTP server transport

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ntp_server.h
 * @brief Value-only clock handoff to an independent IPv4 UDP123 listener.
 */
#pragma once
#include <array>
#include <cstdint>

namespace newmota::platform {

/** @brief Provenance and trust policy, independent of the clock's civil validity. */
enum class NtpServerQuality : std::uint8_t { none, gps, ntp, local, holdover };

/** @brief Immutable clock publication; no callback reads application-owned state. */
struct NtpServerClock final {
    std::uint32_t monotonic_ms{}, utc_seconds{}, reference_seconds{};
    std::uint64_t age_ms{};
    std::uint32_t maximum_age_ms{}, root_delay{}, root_dispersion{};
    std::array<std::uint8_t, 4> reference_id{};
    std::uint16_t millisecond{}, reference_millisecond{};
    std::uint8_t stratum{16}, leap{3};
    NtpServerQuality quality{NtpServerQuality::none};
};

/** @brief Bounded transport diagnostics; counters saturate instead of wrapping. */
struct NtpServerSnapshot final {
    bool listening{}, failed{};
    std::uint32_t requests{}, replies{}, dropped{};
};

/** @brief Evaluates freshness without reading application state. */
[[nodiscard]] NtpServerQuality ntp_server_quality(const NtpServerClock& clock, std::uint32_t now);
/** @brief Stable diagnostic quality name shared with StatusTIM. */
[[nodiscard]] const char* ntp_server_quality_name(NtpServerQuality quality);

/** @brief Publishes a copied clock and requests listener start/stop without waiting.
 * Called only on the application context. The lwIP core owns the PCB, packet
 * processing and arrival/departure timestamps. No DNS/client UDP state is shared.
 */
void ntp_server_poll(const NtpServerClock& clock, bool connected);
/** @brief Returns atomically published native diagnostics without network activity. */
[[nodiscard]] NtpServerSnapshot ntp_server_snapshot();

} // namespace newmota::platform
