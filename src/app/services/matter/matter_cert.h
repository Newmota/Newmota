/*
  matter_cert.h - Matter certificate to X.509 signed data conversion

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

inline constexpr std::size_t kMaxCertificateDer = 700;

/**
 * @brief Rebuilds the DER to-be-signed part of a Matter TLV certificate, which is what its
 * signature covers. The certificate comes from the peer: anything unknown or out of bounds fails.
 * @return The DER size, starting at @p *der inside @p output, or 0 when it is malformed.
 */
[[nodiscard]] std::size_t certificate_signed_data(const std::uint8_t* certificate, std::size_t size,
    std::uint8_t* output, std::size_t capacity, const std::uint8_t** der);

/**
 * @brief Copies the CASE Authenticated Tags of a certificate's subject, in order.
 * @return The count, at most @p capacity, or 0 when there are none or it is malformed.
 */
[[nodiscard]] std::size_t certificate_cats(
    const std::uint8_t* certificate, std::size_t size, std::uint32_t* cats, std::size_t capacity);

} // namespace newmota::matter
