/*
  ds3231.cpp - DS3231 UTC calendar protocol

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file ds3231.cpp
 * @brief Bounded DS3231 calendar operations preserving alarm and square-wave configuration.
 */
#include "drivers/rtc/ds3231.h"
#include "drivers/rtc/calendar_codec.h"
#include "newmota/platform/i2c.h"
#include "services/time/time_service.h"
#include <array>

namespace newmota::rtc {
namespace {
constexpr std::uint8_t kAddress = 0x68;
constexpr std::uint8_t kOscillatorStop = 0x80;

/** @brief Validates Dallas BCD fields and the embedded 12-hour selector. */
bool decode_calendar(std::array<std::uint8_t, 7> raw, std::uint32_t& epoch) {
    const bool twelve_hour = (raw[2] & 0x40) != 0;
    raw[2] &= 0xbf;
    return calendar::decode(raw, calendar::kDallas, twelve_hour, epoch);
}

/** @brief Captures the DS3231 START-latched calendar in one burst, then its validity flags. */
ReadResult read_calendar(
    const std::uint8_t bus, std::uint32_t& epoch, std::uint8_t& status, const bool require_valid) {
    std::array<std::uint8_t, 7> calendar{};
    const std::uint8_t calendar_register = 0;
    auto result = platform::i2c::transfer(
        bus, kAddress, &calendar_register, 1, calendar.data(), calendar.size());
    if (result != platform::i2c::Result::success)
        return result == platform::i2c::Result::nack ? ReadResult::unavailable
                                                     : ReadResult::io_error;
    const std::uint8_t control_register = 0x0e;
    std::array<std::uint8_t, 2> control_status{};
    if (platform::i2c::transfer(bus, kAddress, &control_register, 1, control_status.data(),
            control_status.size()) != platform::i2c::Result::success)
        return ReadResult::io_error;
    status = control_status[1];
    if ((control_status[0] & kOscillatorStop) != 0 ||
        (require_valid && (status & kOscillatorStop) != 0) || !decode_calendar(calendar, epoch))
        return ReadResult::invalid;
    return ReadResult::valid;
}

ReadResult read(const std::uint8_t bus, std::uint32_t& epoch) {
    std::uint8_t status{};
    return read_calendar(bus, epoch, status, true);
}

/** @brief Programs 24-hour UTC and clears OSF only after a verified calendar write. */
WriteResult write(const std::uint8_t bus, const std::uint32_t epoch) {
    const auto civil = TimeService::civil_time(epoch);
    if (epoch < TimeService::kFirstValidTime || civil.year < 2000 || civil.year > 2099)
        return WriteResult::range_error;
    const std::uint8_t control_register = 0x0e;
    std::uint8_t control{};
    if (platform::i2c::transfer(bus, kAddress, &control_register, 1, &control, 1) !=
        platform::i2c::Result::success)
        return WriteResult::io_error;
    if ((control & kOscillatorStop) != 0) {
        const std::array<std::uint8_t, 2> enable{0x0e, static_cast<std::uint8_t>(control & 0x7f)};
        if (platform::i2c::transfer(bus, kAddress, enable.data(), enable.size(), nullptr, 0) !=
            platform::i2c::Result::success)
            return WriteResult::io_error;
    }
    const std::array<std::uint8_t, 8> calendar{0, calendar::bcd(civil.second),
        calendar::bcd(civil.minute), calendar::bcd(civil.hour), civil.day_of_week,
        calendar::bcd(civil.day), calendar::bcd(civil.month),
        calendar::bcd(static_cast<std::uint8_t>(civil.year - 2000))};
    if (platform::i2c::transfer(bus, kAddress, calendar.data(), calendar.size(), nullptr, 0) !=
        platform::i2c::Result::success)
        return WriteResult::io_error;
    std::uint32_t verified{};
    std::uint8_t status{};
    const auto result = read_calendar(bus, verified, status, false);
    if (result == ReadResult::io_error || result == ReadResult::unavailable)
        return WriteResult::io_error;
    if (result != ReadResult::valid || (verified != epoch && verified != epoch + 1U))
        return WriteResult::verify_error;
    if ((status & kOscillatorStop) == 0)
        return WriteResult::success;
    // Alarm flags are W0C: write ones even if a sampled flag was zero. Preserve EN32kHz.
    const std::array<std::uint8_t, 2> clear{
        0x0f, static_cast<std::uint8_t>((status & 0x08) | 0x03)};
    if (platform::i2c::transfer(bus, kAddress, clear.data(), clear.size(), nullptr, 0) !=
        platform::i2c::Result::success)
        return WriteResult::io_error;
    const auto final = read_calendar(bus, verified, status, true);
    if (final == ReadResult::io_error || final == ReadResult::unavailable)
        return WriteResult::io_error;
    return final == ReadResult::valid && (verified == epoch || verified == epoch + 1U)
               ? WriteResult::success
               : WriteResult::verify_error;
}
} // namespace

const Driver ds3231_driver{"DS3231", kAddress, read, write};
} // namespace newmota::rtc
