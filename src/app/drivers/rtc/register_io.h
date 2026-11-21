/*
  register_io.h - Bounded RTC register access

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file register_io.h
 * @brief Shared repeated-START register reads, bounded writes, and calendar verification.
 */
#pragma once
#include "drivers/rtc/calendar_codec.h"
#include "drivers/rtc/rtc_driver.h"
#include "newmota/platform/i2c.h"
#include "services/time/time_service.h"
#include <algorithm>

namespace newmota::rtc::register_io {
/** @brief One configured chip address on one claimed bus; never owns a lease itself. */
struct Device final {
    std::uint8_t bus;
    std::uint8_t address;

    platform::i2c::Result read(std::uint8_t reg, std::uint8_t* data, std::size_t size) const {
        return platform::i2c::transfer(bus, address, &reg, 1, data, size);
    }

    bool read(std::uint8_t reg, std::uint8_t& value) const {
        return read(reg, &value, 1) == platform::i2c::Result::success;
    }

    template <std::size_t N>
    bool write(std::uint8_t reg, const std::array<std::uint8_t, N>& values) const {
        std::array<std::uint8_t, N + 1> bytes{};
        bytes[0] = reg;
        std::copy(values.begin(), values.end(), bytes.begin() + 1);
        return platform::i2c::transfer(bus, address, bytes.data(), bytes.size(), nullptr, 0) ==
               platform::i2c::Result::success;
    }

    bool write(std::uint8_t reg, std::uint8_t value) const {
        return write(reg, std::array<std::uint8_t, 1>{value});
    }
};

inline ReadResult read_error(platform::i2c::Result result) {
    return result == platform::i2c::Result::nack ? ReadResult::unavailable : ReadResult::io_error;
}

/** @brief Bounds the calendar to its supported century before touching any registers. */
inline bool writable(std::uint32_t epoch) {
    return epoch >= utc_calendar::kFirstValidTime && epoch <= 4102444799U;
}

inline calendar::Registers encode(
    std::uint32_t epoch, calendar::Format format, bool twelve_hour = false) {
    const auto civil = TimeService::civil_time(epoch);
    return calendar::encode(format, twelve_hour, civil.year, civil.month, civil.day, civil.hour,
        civil.minute, civil.second, civil.day_of_week);
}

inline WriteResult verify(ReadResult result, std::uint32_t actual, std::uint32_t expected) {
    if (result == ReadResult::io_error || result == ReadResult::unavailable)
        return WriteResult::io_error;
    return result == ReadResult::valid && (actual == expected || actual == expected + 1U)
               ? WriteResult::success
               : WriteResult::verify_error;
}
} // namespace newmota::rtc::register_io
