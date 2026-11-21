/*
  telemetry_settings.h - shared telemetry cadence limits for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstdint>

namespace newmota {

/**
 * @brief Shared Tasmota-compatible telemetry cadence contract.
 *
 * The period belongs to the logical telemetry policy, not to an MQTT broker
 * connection. MQTT is the first consumer, while WebUI, Berry, and later
 * transports may use the same persisted setting and validation rules.
 */
struct TelemetrySettings final {
    static constexpr std::uint16_t kDefaultPeriodSeconds = 300U;
    static constexpr std::uint16_t kMinimumPeriodSeconds = 10U;
    static constexpr std::uint16_t kMaximumPeriodSeconds = 3600U;

    /** @brief Accepts zero (periodic telemetry disabled) or 10..3600 seconds. */
    [[nodiscard]] static constexpr bool valid_period(const std::uint16_t seconds) {
        return seconds == 0U ||
               (seconds >= kMinimumPeriodSeconds && seconds <= kMaximumPeriodSeconds);
    }
};

} // namespace newmota
