/*
  nxp.cpp - BM8563 and NXP PCF85x63 UTC calendar protocols

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file nxp.cpp
 * @brief Related BCD layouts with distinct calendar offsets, hour modes, and stop controls.
 */
#include "drivers/rtc/rtc_chips.h"
#include "drivers/rtc/register_io.h"
#include "newmota_features.h"

namespace newmota::rtc {
namespace {
enum class Model { bm8563, pcf85063, pcf85363 };
constexpr std::uint8_t kAddress = 0x51;

/** @brief Decodes a latched burst without changing flags or power configuration. */
template <Model model>
ReadResult read_calendar(std::uint8_t bus, std::uint32_t& epoch, bool running) {
    const register_io::Device device{bus, kAddress};
    constexpr std::uint8_t offset = model == Model::bm8563 ? 2 : model == Model::pcf85063 ? 4 : 0;
    constexpr auto size = model == Model::pcf85363 ? 8U : 7U;
    std::array<std::uint8_t, size> bytes{};
    const auto result = device.read(offset, bytes.data(), bytes.size());
    if (result != platform::i2c::Result::success)
        return register_io::read_error(result);
    bool twelve_hour = false;
    if constexpr (model == Model::pcf85363) {
        std::uint8_t oscillator{}, function{}, stop{};
        if (!device.read(0x25, oscillator) || !device.read(0x28, function) ||
            !device.read(0x2e, stop))
            return ReadResult::io_error;
        // RTCM selects a different register meaning; STOPM allows TS to stop the counter.
        if ((function & 0x18) != 0 || (running && (stop & 1) != 0))
            return ReadResult::invalid;
        twelve_hour = (oscillator & 0x20) != 0;
    } else {
        std::uint8_t control{};
        if (!device.read(0, control))
            return ReadResult::io_error;
        if ((control & 0x80) != 0 || (running && (control & 0x20) != 0))
            return ReadResult::invalid;
        if constexpr (model == Model::bm8563) {
            if ((control & 0x08) != 0)
                return ReadResult::invalid;
        } else {
            twelve_hour = (control & 2) != 0;
        }
    }
    calendar::Registers raw{};
    std::copy(bytes.begin() + (model == Model::pcf85363 ? 1 : 0), bytes.end(), raw.begin());
    if (running && (raw[0] & 0x80) != 0)
        return ReadResult::invalid;
    raw[0] &= 0x7f;
    if constexpr (model == Model::pcf85363)
        raw[1] &= 0x7f; // EMON is read-only status, not a minute digit.
    // BM8563 uses C=0 for 2000-2099, matching Tasmota's library; C=1 is rejected.
    return calendar::decode(raw, calendar::kNxp, twelve_hour, epoch) ? ReadResult::valid
                                                                     : ReadResult::invalid;
}

template <Model model> ReadResult read(std::uint8_t bus, std::uint32_t& epoch) {
    return read_calendar<model>(bus, epoch, true);
}

/** @brief Keeps STOP asserted after an incomplete or unverifiable calendar write. */
template <Model model> WriteResult write(std::uint8_t bus, std::uint32_t epoch) {
    if (!register_io::writable(epoch))
        return WriteResult::range_error;
    const register_io::Device device{bus, kAddress};
    constexpr std::uint8_t stop_reg = model == Model::pcf85363 ? 0x2e : 0;
    constexpr std::uint8_t stop_bit = model == Model::pcf85363 ? 1 : 0x20;
    std::uint8_t control{};
    if (!device.read(stop_reg, control))
        return WriteResult::io_error;
    bool twelve_hour = false;
    std::uint8_t function{};
    if constexpr (model == Model::pcf85363) {
        std::uint8_t oscillator{};
        if (!device.read(0x25, oscillator) || !device.read(0x28, function))
            return WriteResult::io_error;
        // External stop control is a board policy; do not silently change it.
        if ((function & 8) != 0)
            return WriteResult::verify_error;
        twelve_hour = (oscillator & 0x20) != 0;
    } else {
        // Preserve crystal loading, interrupt enables, and hour mode. Disable test modes only.
        control &= model == Model::bm8563 ? 0x77 : 0x7f;
        if constexpr (model == Model::pcf85063)
            twelve_hour = (control & 2) != 0;
    }
    if (!device.write(stop_reg, static_cast<std::uint8_t>(control | stop_bit)))
        return WriteResult::io_error;
    if constexpr (model == Model::pcf85363) {
        if ((function & 0x10) != 0 &&
            !device.write(0x28, static_cast<std::uint8_t>(function & ~0x10)))
            return WriteResult::io_error;
    }
    const auto raw = register_io::encode(epoch, calendar::kNxp, twelve_hour);
    if constexpr (model == Model::pcf85363) {
        // Address wrap 2f -> 00 implements the datasheet's STOP/CPR/complete-calendar sequence.
        std::array<std::uint8_t, 9> bytes{};
        bytes[0] = 0xa4; // Clear prescaler; no software reset or timestamp reset.
        bytes[1] = 0;    // Hundredths.
        std::copy(raw.begin(), raw.end(), bytes.begin() + 2);
        if (!device.write(0x2f, bytes))
            return WriteResult::io_error;
    } else {
        if (!device.write(model == Model::bm8563 ? 2 : 4, raw))
            return WriteResult::io_error;
    }
    std::uint32_t verified{};
    const auto stopped = read_calendar<model>(bus, verified, false);
    auto verification = register_io::verify(stopped, verified, epoch);
    if (verification != WriteResult::success)
        return verification;
    if (!device.write(stop_reg, static_cast<std::uint8_t>(control & ~stop_bit)))
        return WriteResult::io_error;
    const auto final = read<model>(bus, verified);
    return register_io::verify(final, verified, epoch);
}
} // namespace
#if USE_BM8563
const Driver bm8563_driver{"BM8563", kAddress, read<Model::bm8563>, write<Model::bm8563>};
#endif
#if USE_PCF85063
const Driver pcf85063_driver{"PCF85063", kAddress, read<Model::pcf85063>, write<Model::pcf85063>};
#endif
#if USE_PCF85363
const Driver pcf85363_driver{"PCF85363", kAddress, read<Model::pcf85363>, write<Model::pcf85363>};
#endif
} // namespace newmota::rtc
