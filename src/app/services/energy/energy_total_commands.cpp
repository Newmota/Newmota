/*
  energy_total_commands.cpp - Primary exact lifetime-total compatibility controls

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Command/offset reference: Tasmota xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later. Independent
  exact projection of owned directional accounts; no upstream layout is copied.
*/

/** @file energy_total_commands.cpp
 * @brief Seeds a pre-Today base through an exact offset on the existing lifetime account.
 */
#include "services/energy/energy_service.h"
#include "core/command/command.h"
#include "newmota/platform/platform.h"
#include "support/number.h"
#include "support/text.h"

namespace newmota::energy {
bool Service::seed_total(std::size_t channel, std::string_view payload) {
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
    auto& instance = provider_;
    auto& target = instance.channels[channel - 1];
    /** The source pre-Today base uses live Today, not a dated observation which can
     * deliberately exclude unknown-time consumption or manual undated seeds.
     */
    const auto& today = target.live_daily;
    if (!today.present || today.fault || today.incomplete)
        return false;
    const auto magnitude = wh < 0 ? std::uint64_t(-std::int64_t(wh)) : std::uint64_t(wh);
    SignedEnergy desired{EnergyValue{magnitude, 0}, wh < 0};
    SignedEnergy net{};
    if (!desired.add(today.value.magnitude, today.value.negative) ||
        !net.add(target.account.imported) || !net.add(target.account.exported, true) ||
        !desired.add(net.magnitude, !net.negative))
        return false;
    const auto previous = target.account.total_adjustment;
    const auto previous_fault = target.account.total_fault;
    const auto previous_time = instance.total_time;
    const auto was_dirty = dirty_;
    target.account.total_adjustment = desired;
    target.account.total_fault = false;
    /** An explicit epoch is metadata even without RTC. Default LocalTime/Midnight
     * requires trusted civil time; otherwise use the unknown epoch0,
     * never synthesize a date from the time owner's unsynchronized fallback.
     */
    instance.total_time = comma != std::string_view::npos ? timestamp
                          : !calendar_valid_              ? 0
                          : (today.offset.magnitude.wh || today.offset.magnitude.micro_wh)
                              ? calendar_local_ / 86400 * 86400
                              : calendar_local_;
    dirty_ = true;
    if (save())
        return true;
    target.account.total_adjustment = previous;
    target.account.total_fault = previous_fault;
    instance.total_time = previous_time;
    dirty_ = was_dirty;
    return false;
}

CommandStatus dispatch_total(const CommandRequest& request, CommandOutput& output) {
    if (!support::ascii_iequals(request.name, "EnergyTotal"))
        return CommandStatus::unknown;
    auto& owner = service();
    if (request.is_query())
        owner.poll(platform::monotonic_milliseconds());
    else if (!owner.seed_total(request.has_index ? request.index : 1, request.payload))
        return CommandStatus::failed;
    return owner.write_daily(output, "EnergyTotal") ? CommandStatus::handled
                                                    : CommandStatus::failed;
}
} // namespace newmota::energy
