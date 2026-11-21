/*
  energy_tariff_compat.cpp - Meter-wide source-compatible signed tariff projections

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Command/accounting reference: Tasmota xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later. Independent
  confirmed-delta projections; no upstream floating settings layout is copied.
*/

/** @file energy_tariff_compat.cpp
 * @brief Signed usage, reporting-gated return catch-up and explicit meter-wide tariff seeds.
 */
#include "services/energy/energy_service.h"
#include "services/energy/energy_sum.h"
#include "newmota/platform/platform.h"
#include "support/number.h"
#include "support/text.h"

namespace newmota::energy {
bool TariffUsage::observe(const DirectionalEnergy* delta, std::size_t count, unsigned tariff,
    bool no_export, bool reporting) {
    if (tariff > 2 || count > 32 || (!delta && count))
        return false;
    if (!tariff) {
        for (std::size_t c = 0; c < count; ++c)
            if (delta[c].imported.wh || delta[c].imported.micro_wh || delta[c].exported.wh ||
                delta[c].exported.micro_wh)
                incomplete = true;
        return true;
    }
    const auto index = tariff - 1;
    detail::EnergySum usage_sum{}, return_sum{}, pending_sum{};
    usage_sum.add(usage[index]);
    return_sum.add(returned[index]);
    pending_sum.add(SignedEnergy{pending_export, false});
    if (reporting)
        return_sum.add(SignedEnergy{pending_export, false});
    for (std::size_t c = 0; c < count; ++c) {
        usage_sum.add(SignedEnergy{delta[c].imported, false});
        if (!no_export)
            usage_sum.add(SignedEnergy{delta[c].exported, true});
        if (reporting)
            return_sum.add(SignedEnergy{delta[c].exported, false});
        else
            pending_sum.add(SignedEnergy{delta[c].exported, false});
    }
    SignedEnergy next_usage{}, next_return{};
    if (!usage_sum.value(next_usage) ||
        !(reporting ? return_sum.value(next_return) : pending_sum.value(next_return)) ||
        (!reporting && next_return.negative))
        return false;
    usage[index] = next_usage;
    if (reporting) {
        returned[index] = next_return;
        pending_export = {};
    } else
        pending_export = next_return.magnitude;
    return true;
}

namespace {
/** @brief Streams the fixed two source buckets; unknown or faulted buckets read as zero. */
bool pair(CommandOutput& output, const std::array<SignedEnergy, 2>* values, unsigned places) {
    if (!output.write("["))
        return false;
    for (unsigned i = 0; i < 2; ++i) {
        detail::EnergySum sum{};
        if (values)
            sum.add((*values)[i]);
        if ((i && !output.write(",")) || !sum.write(output, places))
            return false;
    }
    return output.write("]");
}
} // namespace

bool Service::seed_tariff_usage(bool exporting, std::string_view payload) {
    if (storage_fault_ || !provider_.configuration.identity.driver)
        return false;
    std::array<SignedEnergy, 2> values{};
    unsigned count{};
    payload = support::trim_ascii(payload);
    while (!payload.empty()) {
        if (count == 2)
            return false;
        const auto separator = payload.find_first_of(", ");
        std::int32_t wh{};
        if (!support::parse_signed_decimal(payload.substr(0, separator), INT32_MIN, INT32_MAX, wh))
            return false;
        values[count++] = {
            EnergyValue{wh < 0 ? std::uint64_t(-std::int64_t(wh)) : std::uint64_t(wh), 0}, wh < 0};
        if (separator == std::string_view::npos)
            break;
        payload = support::trim_ascii(payload.substr(separator + 1));
        if (payload.empty())
            return false;
    }
    if (!count)
        return false;
    poll(platform::monotonic_milliseconds());
    auto& instance = provider_;
    if (!instance.tariff_usage || instance.tariff_usage_fault)
        return false;
    auto& target = exporting ? instance.tariff_usage->returned : instance.tariff_usage->usage;
    const auto previous = target;
    for (unsigned i = 0; i < count; ++i)
        target[i] = values[i];
    dirty_ = true;
    if (save())
        return true;
    target = previous;
    dirty_ = true;
    return false;
}

bool Service::write_tariff_usage(CommandOutput& output, std::string_view command) const {
    if (!provider_.configuration.identity.driver)
        return false;
    const auto& instance = provider_;
    const auto* data = !instance.tariff_usage_fault ? instance.tariff_usage.get() : nullptr;
    return output.write("{") && output.json_string(command) && output.write(":{\"Usage\":") &&
           pair(output, data ? &data->usage : nullptr, output_resolution(OutputQuantity::energy)) &&
           output.write(",\"Export\":") &&
           pair(output, data ? &data->returned : nullptr,
               output_resolution(OutputQuantity::energy)) &&
           output.write("}}");
}

bool Service::write_tariff_fields(CommandOutput& output, bool exported) const {
    const auto& instance = provider_;
    if (!instance.tariff_schedule.forced &&
        instance.tariff_schedule.minutes[0][0] == instance.tariff_schedule.minutes[1][0])
        return true;
    const auto* data = !instance.tariff_usage_fault ? instance.tariff_usage.get() : nullptr;
    return output.write(exported ? ",\"ExportTariff\":" : ",\"TotalTariff\":") &&
           pair(output, data ? (exported ? &data->returned : &data->usage) : nullptr,
               output_resolution(OutputQuantity::energy));
}

CommandStatus dispatch_tariff_usage(const CommandRequest& request, CommandOutput& output) {
    const bool usage = support::ascii_iequals(request.name, "EnergyUsage");
    const bool exported = support::ascii_iequals(request.name, "EnergyExport");
    if (!usage && !exported)
        return CommandStatus::unknown;
    auto& owner = service();
    if (request.is_query())
        owner.poll(platform::monotonic_milliseconds());
    else if (!owner.seed_tariff_usage(exported, request.payload))
        return CommandStatus::failed;
    return owner.write_tariff_usage(output, usage ? "EnergyUsage" : "EnergyExport")
               ? CommandStatus::handled
               : CommandStatus::failed;
}
} // namespace newmota::energy
