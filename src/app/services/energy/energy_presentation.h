/*
  energy_presentation.h - Retained energy formatting policy without metrology mutation

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Command/default reference: Tasmota support_command.ino and settings.ino,
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
*/

/** @file energy_presentation.h
 * @brief Small retained presentation settings; no Energy driver or runtime resource dependency.
 */
#pragma once
#include <array>
#include <cstdint>

namespace newmota {
class Settings;
class CommandOutput;
struct CommandRequest;
enum class CommandStatus : std::uint8_t;

namespace energy {
/** @brief Indexes the persisted source precisions; factor is always two decimal places. */
enum class OutputQuantity : std::uint8_t {
    energy,
    power,
    voltage,
    current,
    frequency,
    factor,
    weight
};

/** @brief Source defaults and ranges, retained even in firmware without energy formatting. */
struct OutputSettings {
    /** @brief Retained energy/power/voltage/current/frequency/weight decimals, in TLV order. */
    std::array<std::uint8_t, 6> decimals{{3, 0, 0, 3, 0, 0}};

    /** @brief Rejects unknown precision values before persistence or output use. */
    bool valid() const {
        return decimals[0] <= 5 && decimals[1] <= 3 && decimals[2] <= 3 && decimals[3] <= 3 &&
               decimals[4] <= 3 && decimals[5] <= 3;
    }
};

/** @brief Handles one source resolution command, with no account or sample mutation.
 * @param request Query or decimal-place payload; source command suffixes are ignored.
 * @param settings Retained global presentation owner.
 * @param output Response destination.
 * @return unknown for other names; failed for storage/output failure.
 */
CommandStatus dispatch_precision(
    const CommandRequest& request, Settings& settings, CommandOutput& output);
} // namespace energy
} // namespace newmota
