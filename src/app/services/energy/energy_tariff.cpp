/*
  energy_tariff.cpp - Exact directional tariff schedules and commands

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Schedule/command reference: Tasmota xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later. Independent
  exact accounting; no upstream settings layout is copied.
*/

/** @file energy_tariff.cpp
 * @brief Two local-time tariffs using confirmed directional energy advances.
 */
#include "services/energy/energy_service.h"
#include "core/command/command.h"
#include "newmota/platform/platform.h"
#include "support/number.h"
#include "support/text.h"
#include <algorithm>

namespace newmota::energy {
unsigned TariffSchedule::active(std::uint32_t local, bool valid, bool daylight) const {
    if (forced)
        return forced;
    if (!valid)
        return 0;
    const unsigned season = daylight && minutes[0][1] != minutes[1][1] ? 1 : 0;
    const auto start = minutes[0][season], stop = minutes[1][season];
    if (start == stop)
        return 2;
    // Unix epoch Thursday; Sunday=0 and Saturday=6. Weekend applies only to a nonempty interval.
    const auto weekday = (local / 86400 + 4) % 7;
    if (weekend && (weekday == 0 || weekday == 6))
        return 1;
    const auto minute = local % 86400 / 60;
    return (start > stop ? minute >= start || minute < stop : minute >= start && minute < stop) ? 1
                                                                                                : 2;
}

bool TariffData::accrue(const DirectionalEnergy& delta, unsigned tariff) {
    if (tariff > 2)
        return false;
    auto& target = tariff ? totals[tariff - 1] : unassigned;
    auto next = target;
    if (!next.imported.add(delta.imported) || !next.exported.add(delta.exported))
        return false;
    target = next;
    return true;
}

namespace {
/** @brief Trims command separators without allocating a temporary string. */
std::string_view trim(std::string_view text) {
    while (!text.empty() && text.front() == ' ')
        text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ')
        text.remove_suffix(1);
    return text;
}

/** @brief Preserves upstream hours/minutes interpretation and upper-bound clamping. */
bool minute_value(std::string_view text, std::uint16_t& result) {
    const auto colon = text.find(':');
    std::uint32_t number{}, fraction{};
    if (!support::parse_unsigned_decimal(text.substr(0, colon), UINT32_MAX, number))
        return false;
    if (colon != std::string_view::npos &&
        !support::parse_unsigned_decimal(text.substr(colon + 1), UINT32_MAX, fraction))
        return false;
    result = number < 24 ? number * 60 + std::min<std::uint32_t>(fraction, 59)
                         : std::min<std::uint32_t>(number, 1439);
    return true;
}
} // namespace

bool Service::configure_tariff(unsigned index, std::string_view payload) {
    if (storage_fault_ || !provider_.configuration.identity.driver)
        return false;
    auto next = provider_.tariff_schedule;
    payload = trim(payload);
    if (index == 9) {
        std::uint32_t value{};
        if (support::ascii_iequals(payload, "ON"))
            value = 1;
        else if (!support::ascii_iequals(payload, "OFF") &&
                 !support::parse_unsigned_decimal(payload, UINT32_MAX, value))
            return false;
        next.weekend = value & 1;
    } else if (index == 1 || index == 2) {
        if (support::ascii_iequals(payload, "ON") || payload == "1")
            next.forced = index;
        else if (support::ascii_iequals(payload, "OFF") || payload == "0")
            next.forced = 0;
        else {
            unsigned count = 0;
            while (!payload.empty()) {
                if (count == 2)
                    return false;
                const auto separator = payload.find_first_of(", ");
                if (!minute_value(payload.substr(0, separator), next.minutes[index - 1][count++]))
                    return false;
                if (separator == std::string_view::npos)
                    break;
                const bool comma = payload[separator] == ',';
                payload = trim(payload.substr(separator + 1));
                if (comma && payload.empty())
                    return false;
            }
            if (!count)
                return false;
        }
    } else
        return false;
    /** Account through command time under the previous schedule before changing its boundaries. */
    poll(platform::monotonic_milliseconds());
    auto& current = provider_.tariff_schedule;
    const auto previous = current;
    current = next;
    dirty_ = true;
    if (save())
        return true;
    current = previous;
    dirty_ = true;
    return false;
}

bool Service::write_tariff(CommandOutput& output) const {
    if (!provider_.configuration.identity.driver)
        return false;
    const auto& schedule = provider_.tariff_schedule;
    return output.format(
        "{\"Tariff\":{\"Off-Peak\":{\"STD\":\"%02u:%02u\",\"DST\":\"%02u:%02u\"},"
        "\"Standard\":{\"STD\":\"%02u:%02u\",\"DST\":\"%02u:%02u\"},\"Weekend\":\"%s\","
        "\"Forced\":\"%u\"}}",
        schedule.minutes[0][0] / 60, schedule.minutes[0][0] % 60, schedule.minutes[0][1] / 60,
        schedule.minutes[0][1] % 60, schedule.minutes[1][0] / 60, schedule.minutes[1][0] % 60,
        schedule.minutes[1][1] / 60, schedule.minutes[1][1] % 60, schedule.weekend ? "ON" : "OFF",
        unsigned(schedule.forced));
}
} // namespace newmota::energy
