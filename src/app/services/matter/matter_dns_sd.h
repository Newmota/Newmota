/*
  matter_dns_sd.h - minimal DNS-SD responder records for Matter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace newmota::matter {

inline constexpr std::size_t kDnsServices = 6;
inline constexpr std::size_t kDnsSubtypes = 4;
inline constexpr std::size_t kDnsAddresses = 3;

/** @brief One advertised instance of _matterc._udp or _matter._tcp. */
struct DnsService {
    char instance[34]{};
    bool commissionable{};
    char subtypes[kDnsSubtypes][20]{}; // For example _L3840, without ._sub.
    char txt[128]{};                   // Newline-separated key=value items.
};

/** @brief Everything the responder may publish. */
struct DnsRecords {
    char host[24]{};
    std::uint16_t port{5540};
    std::size_t service_count{};
    DnsService services[kDnsServices]{};
    std::size_t address_count{};
    std::array<std::array<std::uint8_t, 16>, kDnsAddresses> addresses{};
};

/**
 * @brief Answers one DNS-SD query. Names, pointers and counts in the query are untrusted.
 * @return The response size, or 0 when the packet is malformed or no record matches.
 */
[[nodiscard]] std::size_t dns_sd_respond(const std::uint8_t* query, std::size_t query_size,
    const DnsRecords& records, std::uint8_t* output, std::size_t capacity);

/** @brief Builds an unsolicited announcement of every record, or 0 when there are none. */
[[nodiscard]] std::size_t dns_sd_announce(
    const DnsRecords& records, std::uint8_t* output, std::size_t capacity);

} // namespace newmota::matter
