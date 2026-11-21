/*
  energy_export_commands.cpp - Runtime local-export reporting and exact seed controls

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Command/reporting reference: Tasmota xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later. Independent
  projection; no upstream pointer or floating settings layout is copied.
*/

/** @file energy_export_commands.cpp
 * @brief Separates volatile reporting enable, durable export seeds and device direction routing.
 */
#include "services/energy/energy_service.h"
#include "core/command/command.h"
#include "newmota/platform/platform.h"
#include "support/number.h"
#include "support/text.h"

namespace newmota::energy {
bool Service::seed_export(std::size_t channel, std::string_view payload, std::uint32_t generation) {
    if (storage_fault_ || !channel || channel > provider_.configuration.channels)
        return false;
    const auto comma = payload.find(',');
    std::int32_t wh{};
    std::uint32_t timestamp{};
    if (!support::parse_signed_decimal(
            support::trim_ascii(payload.substr(0, comma)), INT32_MIN, INT32_MAX, wh) ||
        (comma != std::string_view::npos &&
            !support::parse_unsigned_decimal(
                support::trim_ascii(payload.substr(comma + 1)), UINT32_MAX, timestamp)))
        return false;
    poll(platform::monotonic_milliseconds());
    /** @brief Poll can complete device control and replace an identity; revalidate before seeding.
     */
    const auto& configuration = provider_.configuration;
    if (storage_fault_ || !configuration.identity.driver || channel > configuration.channels ||
        (generation && provider_.generation != generation) || !export_reporting())
        return false;
    auto& instance = provider_;
    auto& account = instance.channels[channel - 1].account;
    const auto magnitude = wh < 0 ? std::uint64_t(-std::int64_t(wh)) : std::uint64_t(wh);
    SignedEnergy adjustment{EnergyValue{magnitude, 0}, wh < 0};
    if (!adjustment.add(account.exported, true))
        return false;
    const auto previous = account.export_adjustment;
    const auto previous_time = instance.total_time;
    account.export_adjustment = adjustment;
    if (comma != std::string_view::npos)
        instance.total_time = timestamp;
    dirty_ = true;
    if (save())
        return true;
    account.export_adjustment = previous;
    instance.total_time = previous_time;
    dirty_ = true;
    return false;
}

namespace {
/** @brief Preserves the source low-bit toggle and accepts standard ON/OFF command spelling. */
bool reporting_value(std::string_view payload, bool& value) {
    payload = support::trim_ascii(payload);
    if (support::ascii_iequals(payload, "ON")) {
        value = true;
        return true;
    }
    if (support::ascii_iequals(payload, "OFF")) {
        value = false;
        return true;
    }
    std::uint32_t number{};
    if (!support::parse_unsigned_decimal(payload, UINT32_MAX, number))
        return false;
    value = number & 1;
    return true;
}
} // namespace

CommandStatus dispatch_export(const CommandRequest& request, CommandOutput& output) {
    if (!support::ascii_iequals(request.name, "EnergyExportActive"))
        return CommandStatus::unknown;
    auto& owner = service();
    Snapshot configured{};
    if (!owner.snapshot(1, configured))
        return CommandStatus::invalid;
    if (request.is_query())
        owner.poll(platform::monotonic_milliseconds());
    else if (!request.has_index) {
        bool enabled{};
        if (!reporting_value(request.payload, enabled))
            return CommandStatus::invalid;
        if (!owner.set_export_reporting(enabled))
            return CommandStatus::failed;
    } else if (owner.export_reporting() && !owner.seed_export(request.index, request.payload))
        return CommandStatus::failed; // Upstream ignores an indexed seed while reporting is off.
    if (!owner.export_reporting())
        return output.write("{\"EnergyExportActive\":\"OFF\"}") ? CommandStatus::handled
                                                                : CommandStatus::failed;
    return owner.write_daily(output, "EnergyExportActive") ? CommandStatus::handled
                                                           : CommandStatus::failed;
}
} // namespace newmota::energy
