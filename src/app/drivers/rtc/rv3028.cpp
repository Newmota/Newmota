/*
  rv3028.cpp - Micro Crystal RV-3028-C7 UTC calendar protocol

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file rv3028.cpp
 * @brief Coherent calendar access and verified, write-if-changed backup switching setup.
 */
#include "drivers/rtc/rtc_chips.h"
#include "drivers/rtc/register_io.h"
#include "newmota/platform/platform.h"

namespace newmota::rtc {
namespace {
constexpr std::uint8_t kAddress = 0x52;

/** @brief Waits for EEPROM with a fixed deadline, including failed or stalled devices. */
bool wait_eeprom(const register_io::Device& device) {
    const auto started = platform::monotonic_milliseconds();
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        std::uint8_t status{};
        if (!device.read(0x0e, status))
            return false;
        if ((status & 0x80) == 0)
            return true;
        if (static_cast<std::uint32_t>(platform::monotonic_milliseconds() - started) >= 100)
            return false;
        platform::sleep_for_milliseconds(1);
    }
    return false;
}

bool eeprom_command(const register_io::Device& device, std::uint8_t command) {
    if (!device.write(0x27, 0) || !device.write(0x27, command))
        return false;
    platform::sleep_for_milliseconds(command == 0x21 ? 10 : 1);
    return wait_eeprom(device);
}

/** @brief Adds DSM only when backup switching is disabled, retaining charger and calibration. */
std::uint8_t backup_enabled(std::uint8_t value) {
    if ((value & 4) == 0)
        value = static_cast<std::uint8_t>((value & ~0x0c) | 4);
    return static_cast<std::uint8_t>(value | 0x10); // FEDE is required by the manufacturer.
}

bool prepare(std::uint8_t bus) {
    const register_io::Device device{bus, kAddress};
    std::uint8_t control{}, mirror{};
    if (!device.read(0x0f, control) || !wait_eeprom(device) || !device.read(0x37, mirror))
        return false;
    if (!device.write(0x0f, static_cast<std::uint8_t>(control | 8)))
        return false;
    // Restore the caller's auto-refresh policy on every exit, including failed EEPROM commands.
    const auto setup = [&]() {
        if (!wait_eeprom(device) ||
            !device.write(0x37, static_cast<std::uint8_t>(mirror & ~0x0c)) ||
            !device.write(0x25, 0x37) || !eeprom_command(device, 0x22))
            return false;
        std::uint8_t saved{};
        if (!device.read(0x26, saved))
            return false;
        const auto desired = backup_enabled(saved);
        if (desired != saved) {
            if (!device.write(0x26, desired) || !eeprom_command(device, 0x21) ||
                !eeprom_command(device, 0x22))
                return false;
            std::uint8_t verified{};
            if (!device.read(0x26, verified) || verified != desired)
                return false;
        }
        const auto active = backup_enabled(mirror);
        if (!device.write(0x37, active))
            return false;
        std::uint8_t verified{};
        return device.read(0x37, verified) && verified == active;
    };
    const bool success = setup();
    // A failed EEPROM operation must not leave a previously configured backup disabled.
    const bool restored_backup = success || device.write(0x37, mirror);
    const bool restored_refresh = device.write(0x0f, control);
    return success && restored_backup && restored_refresh;
}

ReadResult read_calendar(std::uint8_t bus, std::uint32_t& epoch, bool require_valid) {
    const register_io::Device device{bus, kAddress};
    calendar::Registers raw{};
    const auto result = device.read(0, raw.data(), raw.size());
    if (result != platform::i2c::Result::success)
        return register_io::read_error(result);
    std::uint8_t status{}, control{};
    if (!device.read(0x0e, status) || !device.read(0x10, control))
        return ReadResult::io_error;
    if (require_valid && (status & 1) != 0)
        return ReadResult::invalid;
    return calendar::decode(raw, calendar::kMicroCrystal, (control & 2) != 0, epoch)
               ? ReadResult::valid
               : ReadResult::invalid;
}

ReadResult read(std::uint8_t bus, std::uint32_t& epoch) {
    return read_calendar(bus, epoch, true);
}

WriteResult write(std::uint8_t bus, std::uint32_t epoch) {
    if (!register_io::writable(epoch))
        return WriteResult::range_error;
    const register_io::Device device{bus, kAddress};
    std::uint8_t control{};
    if (!device.read(0x10, control) ||
        !device.write(0, register_io::encode(epoch, calendar::kMicroCrystal, (control & 2) != 0)))
        return WriteResult::io_error;
    std::uint32_t verified{};
    const auto programmed = read_calendar(bus, verified, false);
    const auto verification = register_io::verify(programmed, verified, epoch);
    if (verification != WriteResult::success)
        return verification;
    // All other status flags are W0C; never clear a newly arrived alarm or backup event.
    if (!device.write(0x0e, 0x7e))
        return WriteResult::io_error;
    const auto final = read(bus, verified);
    return register_io::verify(final, verified, epoch);
}
} // namespace

const Driver rv3028_driver{"RV3028", kAddress, read, write, prepare};
} // namespace newmota::rtc
