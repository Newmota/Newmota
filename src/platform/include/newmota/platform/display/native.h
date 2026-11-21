/*
  native.h - native SPI display transport contract

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file native.h
 * @brief Synchronous native SPI hooks used by the retained shared display owner.
 */
#pragma once
#include "descriptor.h"

namespace newmota::platform::display::detail {
/** @brief Checks native routes, rate and framing without changing hardware. */
Result native_spi_preflight(const Config&);
/** @brief Configures one reserved native SPI bus, retaining it until restart. */
Result native_spi_open(const Config&);
/** @brief Transmits a complete byte span and drains the wire before returning. */
Result native_spi_write(bool data, const std::uint8_t*, std::size_t);
/** @brief Validates native quad routes and rate without touching hardware. */
Result native_qspi_preflight(const Config&);
/** @brief Opens a native quad transport after the caller reserves its pins.
 * Resources remain retained until restart, including after a hardware failure.
 */
Result native_qspi_open(const Config&);
/**
 * @brief Sends complete CS frames and drains the wire before returning.
 * Command/address phases use one lane: 02/32 followed by 00, opcode, 00.
 * Backends may split pixel spans into RAMWRC frames at their native limit.
 * Parameters use one lane; RGB565 pixel bytes use four lanes, MSB first.
 * @param pixels Selects four-lane pixel data instead of one-lane parameters.
 * @param opcode DCS command, including RAMWR or RAMWRC for each pixel frame.
 * @return success once the source buffer can be reused; otherwise a transport error.
 */
Result native_qspi_write(bool pixels, std::uint8_t opcode, const std::uint8_t*, std::size_t);
} // namespace newmota::platform::display::detail
