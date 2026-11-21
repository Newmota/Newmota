/*
  i2c_commands.cpp - shared I2C diagnostic commands for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file i2c_commands.cpp
 * @brief Bounded diagnostics over the shared platform I2C owner.
 */

#include "drivers/i2c/i2c_commands.h"
#include "newmota/platform/i2c.h"
#include "support/text.h"

#include <array>

namespace newmota {
namespace {
constexpr std::uint8_t kFirstScanAddress = 0x08;
constexpr std::uint8_t kLastScanAddress = 0x77;

/** @brief Retains one bus diagnostic until the complete response can be sized. */
struct ScanResult final {
    std::array<std::uint8_t, kLastScanAddress - kFirstScanAddress + 1> addresses{};
    std::size_t count{};
    std::uint8_t bus{}, error{}, failed_address{};
};

/** @brief Probes nonreserved addresses; a bus error supersedes earlier discoveries. */
void scan_bus(ScanResult& scan) {
    for (std::uint8_t address = kFirstScanAddress; address <= kLastScanAddress; ++address) {
        const auto result = platform::i2c::transfer(scan.bus, address, nullptr, 0, nullptr, 0);
        if (result == platform::i2c::Result::success) {
            scan.addresses[scan.count++] = address;
        } else if (result != platform::i2c::Result::nack) {
            scan.error = result == platform::i2c::Result::timeout ? 5 : 4;
            scan.failed_address = address;
            break;
        }
    }
}

/** @brief Writes a complete scan using fragments that fit CommandOutput::format. */
[[nodiscard]] bool write_scan_results(CommandOutput& output,
    const std::array<ScanResult, platform::i2c::kBusCount>& scans, const std::size_t count) {
    if (count == 0)
        return output.write("{\"I2CScan\":\"No enabled buses\"}");
    if (count > 1 && !output.write("{\"Results\":["))
        return false;
    for (std::size_t index = 0; index < count; ++index) {
        const auto& scan = scans[index];
        if (index != 0 && !output.write(","))
            return false;
        if (scan.error != 0) {
            if (!output.format("{\"I2CScan\":\"Error %u at 0x%02x on bus%u\"}",
                    unsigned(scan.error), unsigned(scan.failed_address), unsigned(scan.bus + 1)))
                return false;
        } else if (scan.count == 0) {
            if (!output.format(
                    "{\"I2CScan\":\"No devices found on bus%u\"}", unsigned(scan.bus + 1)))
                return false;
        } else {
            if (!output.format(
                    "{\"I2CScan\":\"Device(s) found on bus%u at", unsigned(scan.bus + 1)))
                return false;
            for (std::size_t address = 0; address < scan.count; ++address) {
                if (!output.format(" 0x%02x", unsigned(scan.addresses[address])))
                    return false;
            }
            if (!output.write("\"}"))
                return false;
        }
    }
    return count == 1 || output.write("]}");
}

/** @brief Counts serialized bytes without writing a partial response to a caller. */
[[nodiscard]] bool count_output(void*, std::string_view) {
    return true;
}
} // namespace

namespace commands {
/** @brief Executes I2CScan after preflighting its complete serialized response. */
CommandStatus dispatch_i2c(const CommandRequest& request, CommandOutput& output) {
    if (!support::ascii_iequals(request.name, "I2CScan"))
        return CommandStatus::unknown;

    std::array<ScanResult, platform::i2c::kBusCount> scans{};
    std::size_t count = 0;
    for (std::uint8_t bus = 0; bus < platform::i2c::kBusCount; ++bus) {
        if (!platform::i2c::available(bus))
            continue;
        auto& scan = scans[count++];
        scan.bus = bus;
        scan_bus(scan);
    }

    CommandOutput measured{count_output, nullptr};
    if (!write_scan_results(measured, scans, count) ||
        measured.size() > kMaximumCommandResponseLength) {
        (void)output.write("{\"I2CScan\":\"Response exceeded limit\"}");
        return CommandStatus::failed;
    }
    (void)write_scan_results(output, scans, count);
    return CommandStatus::handled;
}
} // namespace commands
} // namespace newmota
