/*
  power_telemetry.h - canonical power telemetry serialization for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "newmota_features.h"

#include <cstddef>
#include <cstdint>

namespace newmota {

class CommandOutput;
class PowerService;

/**
 * @brief Writes the implemented relay fields without a JSON object wrapper.
 *
 * This is used by StatusSTS and by the full canonical STATE serializer so
 * relay naming and ON/OFF value semantics remain one contract.
 * @param first false when fields precede these, so the first relay field needs a comma.
 */
[[nodiscard]] bool write_power_state_fields(
    CommandOutput& output, const PowerService& power, bool first = true);

#if USE_SLEEP
/** @brief Writes Tasmota's SleepMode, Sleep and LoadAvg fields, as in STATE. Safeboot never
 * sleeps and reports none of them. */
[[nodiscard]] bool write_sleep_state_fields(CommandOutput& output);
#endif

/**
 * @brief Serializes the canonical relay STATE value into caller storage.
 *
 * The payload is deliberately bounded and contains no web-specific mask or
 * presentation fields.  A transport may publish the resulting bytes without
 * constructing another sensor document.
 */
[[nodiscard]] bool serialize_power_state(const PowerService& power, std::uint32_t uptime_seconds,
    char* output, std::size_t capacity, std::size_t& size);

} // namespace newmota
