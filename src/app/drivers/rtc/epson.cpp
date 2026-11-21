/*
  epson.cpp - Epson RX8010, RX8025T, and RX8130CE UTC calendars

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file epson.cpp
 * @brief Coherent Epson calendar bursts with model-specific STOP and validity registers.
 */
#include "drivers/rtc/rtc_chips.h"
#include "drivers/rtc/register_io.h"
#include "newmota_features.h"

namespace newmota::rtc {
namespace {
enum class Model { rx8010, rx8025, rx8130 };
constexpr std::uint8_t kAddress = 0x32;
template <Model model> constexpr std::uint8_t kCalendar = model == Model::rx8025 ? 0 : 0x10;
template <Model model>
constexpr std::uint8_t kFlag = model == Model::rx8010   ? 0x1e
                               : model == Model::rx8025 ? 0x0e
                                                        : 0x1d;
template <Model model>
constexpr std::uint8_t kControl = model == Model::rx8010   ? 0x1f
                                  : model == Model::rx8025 ? 0x0f
                                                           : 0x1e;
template <Model model> constexpr std::uint8_t kStop = model == Model::rx8025 ? 1 : 0x40;

template <Model model>
ReadResult read_calendar(std::uint8_t bus, std::uint32_t& epoch, bool running) {
    const register_io::Device device{bus, kAddress};
    calendar::Registers raw{};
    const auto result = device.read(kCalendar<model>, raw.data(), raw.size());
    if (result != platform::i2c::Result::success)
        return register_io::read_error(result);
    std::uint8_t flags{}, control{};
    if (!device.read(kFlag<model>, flags) || !device.read(kControl<model>, control))
        return ReadResult::io_error;
    if constexpr (model == Model::rx8025) {
        std::uint8_t extension{};
        if (!device.read(0x0d, extension))
            return ReadResult::io_error;
        if ((extension & 0x80) != 0)
            return ReadResult::invalid;
    } else if ((control & 0x80) != 0) {
        return ReadResult::invalid;
    }
    // VDET on RX8025T also means temperature compensation stopped: do not trust its age.
    constexpr std::uint8_t invalid_flags = model == Model::rx8025 ? 3 : 2;
    if (running && ((flags & invalid_flags) != 0 || (control & kStop<model>) != 0))
        return ReadResult::invalid;
    return calendar::decode(raw, calendar::kEpson, false, epoch) ? ReadResult::valid
                                                                 : ReadResult::invalid;
}

template <Model model> ReadResult read(std::uint8_t bus, std::uint32_t& epoch) {
    return read_calendar<model>(bus, epoch, true);
}

/** @brief Sets only mandatory reserved bytes, retaining digital calibration and IRQ routing. */
inline bool prepare_rx8010(const register_io::Device& device) {
    return device.write(0x17, 0xd8) && device.write(0x31, 0x08);
}

/** @brief Enables the RX8130 backup switch latch without enabling its charger.
 * INIEN must become one at least once after POR; restore the configured interface mode.
 * VDD must be above VDET1 during this sequence, since INIEN=1 gates I2C below that level.
 */
#if USE_RX8130
bool enable_rx8130_backup(const register_io::Device& device) {
    std::uint8_t control{};
    if (!device.read(0x1f, control))
        return false;
    if ((control & 0x10) != 0)
        return true;
    // CHGEN may have been left set while INIEN=0; do not activate charging while toggling INIEN.
    const auto initialized = static_cast<std::uint8_t>((control & 0xd7) | 0x10);
    const auto restored = static_cast<std::uint8_t>(control & 0xf7);
    std::uint8_t verified{};
    const bool enabled =
        device.write(0x1f, initialized) && device.read(0x1f, verified) && verified == initialized;
    // Restore the interface/charger policy even if initialization could not be verified.
    const bool restore_ok =
        device.write(0x1f, restored) && device.read(0x1f, verified) && verified == restored;
    return enabled && restore_ok;
}
#endif

template <Model model> WriteResult write(std::uint8_t bus, std::uint32_t epoch) {
    if (!register_io::writable(epoch))
        return WriteResult::range_error;
    const register_io::Device device{bus, kAddress};
    std::uint8_t control{};
    if (!device.read(kControl<model>, control))
        return WriteResult::io_error;
    control &= model == Model::rx8025 ? 0xf8 : 0x7f; // Reserved bits or TEST.
    if (!device.write(kControl<model>, static_cast<std::uint8_t>(control | kStop<model>)))
        return WriteResult::io_error;
    if constexpr (model == Model::rx8010) {
        if (!prepare_rx8010(device))
            return WriteResult::io_error;
    } else if constexpr (model == Model::rx8025) {
        std::uint8_t extension{};
        if (!device.read(0x0d, extension) ||
            !device.write(0x0d, static_cast<std::uint8_t>(extension & 0x7f)))
            return WriteResult::io_error;
    }
    if (!device.write(kCalendar<model>, register_io::encode(epoch, calendar::kEpson)))
        return WriteResult::io_error;
    std::uint32_t verified{};
    const auto stopped = read_calendar<model>(bus, verified, false);
    const auto verification = register_io::verify(stopped, verified, epoch);
    if (verification != WriteResult::success)
        return verification;
    // W0C flags: ones retain pending alarms/timers, including events after the last read.
    constexpr std::uint8_t retained_flags = model == Model::rx8130 ? 0xbc : 0x38;
    if (!device.write(kFlag<model>, retained_flags) ||
        !device.write(kControl<model>, static_cast<std::uint8_t>(control & ~kStop<model>)))
        return WriteResult::io_error;
    const auto final = read<model>(bus, verified);
    return register_io::verify(final, verified, epoch);
}
} // namespace
#if USE_RX8010
const Driver rx8010_driver{"RX8010", kAddress, read<Model::rx8010>, write<Model::rx8010>,
    [](std::uint8_t bus) { return prepare_rx8010({bus, kAddress}); }};
#endif
#if USE_RX8025
const Driver rx8025_driver{"RX8025T", kAddress, read<Model::rx8025>, write<Model::rx8025>};
#endif
#if USE_RX8130
const Driver rx8130_driver{"RX8130CE", kAddress, read<Model::rx8130>, write<Model::rx8130>,
    [](std::uint8_t bus) { return enable_rx8130_backup({bus, kAddress}); }};
#endif
} // namespace newmota::rtc
