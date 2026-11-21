/*
  ds1307.cpp - DS1307 UTC calendar protocol

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ds1307.cpp
 * @brief DS1307 calendar bursts; its SRAM and control register are never written.
 */
#include "drivers/rtc/rtc_chips.h"
#include "drivers/rtc/register_io.h"

namespace newmota::rtc {
namespace {
constexpr std::uint8_t kAddress = 0x68;

ReadResult read_calendar(std::uint8_t bus, std::uint32_t& epoch, bool running) {
    calendar::Registers raw{};
    const auto result = register_io::Device{bus, kAddress}.read(0, raw.data(), raw.size());
    if (result != platform::i2c::Result::success)
        return register_io::read_error(result);
    if (running && (raw[0] & 0x80) != 0)
        return ReadResult::invalid;
    raw[0] &= 0x7f;
    const bool twelve_hour = (raw[2] & 0x40) != 0;
    raw[2] &= 0xbf;
    return calendar::decode(raw, calendar::kDallas, twelve_hour, epoch) ? ReadResult::valid
                                                                        : ReadResult::invalid;
}

ReadResult read(std::uint8_t bus, std::uint32_t& epoch) {
    return read_calendar(bus, epoch, true);
}

WriteResult write(std::uint8_t bus, std::uint32_t epoch) {
    if (!register_io::writable(epoch))
        return WriteResult::range_error;
    const register_io::Device device{bus, kAddress};
    std::uint8_t seconds{};
    if (!device.read(0, seconds) || !device.write(0, static_cast<std::uint8_t>(seconds | 0x80)))
        return WriteResult::io_error;
    auto raw = register_io::encode(epoch, calendar::kDallas);
    raw[0] |= 0x80;
    if (!device.write(0, raw))
        return WriteResult::io_error;
    std::uint32_t verified{};
    const auto stopped = read_calendar(bus, verified, false);
    const auto verification = register_io::verify(stopped, verified, epoch);
    if (verification != WriteResult::success)
        return verification;
    if (!device.write(0, static_cast<std::uint8_t>(raw[0] & 0x7f)))
        return WriteResult::io_error;
    const auto result = read(bus, verified);
    return register_io::verify(result, verified, epoch);
}
} // namespace

const Driver ds1307_driver{"DS1307", kAddress, read, write};
} // namespace newmota::rtc
