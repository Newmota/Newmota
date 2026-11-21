/*
  energy_commands.cpp - Routed bounded energy configuration and readings

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file energy_commands.cpp
 * @brief Routed bounded energy configuration and readings.
 */

#include "services/energy/energy_service.h"
#include "core/command/command.h"
#include "support/number.h"
#include "support/text.h"

namespace newmota::energy {
namespace {
/** @brief Parses decimal references, truncating fractions in each driver's native units. */
bool primary_reference(std::string_view text, bool current, std::uint32_t& reference,
    std::uint32_t current_scale = 1) {
    const auto dot = text.find('.');
    const auto whole = text.substr(0, dot);
    std::uint32_t integer{};
    const std::uint32_t scale = current ? current_scale : 1000;
    if (!support::parse_unsigned_decimal(whole, INT32_MAX / scale, integer))
        return false;
    std::uint32_t fraction{};
    if (dot != std::string_view::npos) {
        const auto decimals = text.substr(dot + 1);
        if (decimals.empty())
            return false;
        auto place = scale;
        for (const auto digit : decimals) {
            if (digit < '0' || digit > '9')
                return false;
            if (place > 1) {
                place /= 10;
                fraction += static_cast<std::uint32_t>(digit - '0') * place;
            }
        }
    }
    if (integer * scale > INT32_MAX - fraction)
        return false;
    reference = integer * scale + fraction;
    return reference != 0;
}

/** @brief Parses a signed native reference while retaining canonical milli-units. */
bool signed_reference(std::string_view text, bool current, std::uint32_t& reference) {
    const bool negative = !text.empty() && text.front() == '-';
    if (negative)
        text.remove_prefix(1);
    if (!primary_reference(text, current, reference))
        return false;
    if (negative)
        reference = std::uint32_t(-static_cast<std::int32_t>(reference));
    return true;
}
} // namespace

CommandStatus dispatch(const CommandRequest& request, CommandOutput& output) {
#if USE_ENERGY_MAX_ENERGY
    const auto maximum_energy = service().max_energy_command(request, output);
    if (maximum_energy != CommandStatus::unknown)
        return maximum_energy;
#endif
#if USE_ENERGY_MAX_POWER
    const auto maximum = service().max_power_command(request, output);
    if (maximum != CommandStatus::unknown)
        return maximum;
#endif
#if USE_ENERGY_MARGINS
    const auto margin = service().margin_command(request, output);
    if (margin != CommandStatus::unknown)
        return margin;
#endif
#if USE_ENERGY_POWER_DELTA
    if (support::ascii_iequals(request.name, "PowerDelta"))
        return service().power_delta_command(request, output);
#endif
#if USE_ENERGY_TARIFF_COMPAT
    const auto tariff_status = dispatch_tariff_usage(request, output);
    if (tariff_status != CommandStatus::unknown)
        return tariff_status;
#endif
#if USE_ENERGY_EXPORT_COMPAT
    const auto export_status = dispatch_export(request, output);
    if (export_status != CommandStatus::unknown)
        return export_status;
#endif
#if USE_ENERGY_TOTAL_COMPAT
    const auto total = dispatch_total(request, output);
    if (total != CommandStatus::unknown)
        return total;
#endif
#if USE_ENERGY_DAILY_COMPAT
    const auto daily = dispatch_daily(request, output);
    if (daily != CommandStatus::unknown)
        return daily;
#endif
    const bool primary_power_cal = support::ascii_iequals(request.name, "PowerCal");
    const bool primary_voltage_cal = support::ascii_iequals(request.name, "VoltageCal");
    const bool primary_current_cal = support::ascii_iequals(request.name, "CurrentCal");
    const bool primary_frequency_cal = support::ascii_iequals(request.name, "FrequencyCal");
    const bool primary_power_set = support::ascii_iequals(request.name, "PowerSet");
    const bool primary_voltage_set = support::ascii_iequals(request.name, "VoltageSet");
    const bool primary_current_set = support::ascii_iequals(request.name, "CurrentSet");
    const bool primary_frequency_set = support::ascii_iequals(request.name, "FrequencySet");
    const bool primary_cal =
        primary_power_cal || primary_voltage_cal || primary_current_cal || primary_frequency_cal;
    const bool primary_set =
        primary_power_set || primary_voltage_set || primary_current_set || primary_frequency_set;
    const bool config = support::ascii_iequals(request.name, "EnergyConfig");
    const bool module_address = support::ascii_iequals(request.name, "ModuleAddress");
    const bool range_config = config && request.payload.size() > 6 &&
                              support::ascii_iequals(request.payload.substr(0, 6), "range=");
#if USE_ENERGY_TARIFF
    if (support::ascii_iequals(request.name, "Tariff")) {
        if (!request.is_query() &&
            !service().configure_tariff(request.has_index ? request.index : 1, request.payload))
            return CommandStatus::failed;
        return service().write_tariff(output) ? CommandStatus::handled : CommandStatus::failed;
    }
#endif
    if (!config && !module_address && !primary_cal && !primary_set)
        return CommandStatus::unknown;
    auto& owner = service();
#if USE_ENERGY_DUMMY
    const auto dummy = owner.dummy_config_command(request, output);
    if (dummy != CommandStatus::unknown)
        return dummy;
#endif
    // Tasmota command indices select a phase of the provider, never a meter.
    const std::size_t phase = request.has_index ? request.index : 1;
    if (primary_cal || primary_set) {
        const auto field =
            (primary_power_cal || primary_power_set)       ? CalibrationField::power
            : (primary_voltage_cal || primary_voltage_set) ? CalibrationField::voltage
            : (primary_current_cal || primary_current_set) ? CalibrationField::current
                                                           : CalibrationField::frequency;
        std::uint32_t supported{};
        if (!owner.calibration_value(phase ? phase : 1, field, supported))
            return CommandStatus::failed;
        if (!request.is_query()) {
            const auto comma = request.payload.find(',');
            std::uint32_t value{};
            if (primary_set && comma != std::string_view::npos) {
                std::uint32_t voltage{};
                if ((!primary_power_set && !primary_current_set) ||
                    request.payload.find(',', comma + 1) != std::string_view::npos ||
                    !primary_reference(request.payload.substr(0, comma), false, value) ||
                    !primary_reference(request.payload.substr(comma + 1), false, voltage))
                    return CommandStatus::invalid;
                if (!owner.set_resistive_reference(phase, value, voltage))
                    return CommandStatus::failed;
            } else if (primary_set) {
                const auto current_scale =
                    primary_current_set && !owner.primary_current_in_amperes()
                        ? owner.current_reference_scale()
                        : 1;
                if (!(owner.signed_calibration()
                            ? signed_reference(request.payload,
                                  primary_current_set && !owner.primary_current_in_amperes(), value)
                            : primary_reference(request.payload,
                                  primary_current_set && !owner.primary_current_in_amperes(), value,
                                  current_scale)))
                    return CommandStatus::invalid;
                if (!owner.set_reference(field, value, phase, current_scale))
                    return CommandStatus::failed;
            } else {
                if (owner.signed_calibration()) {
                    std::int32_t native{};
                    if (!support::parse_signed_decimal(
                            request.payload, INT32_MIN, INT32_MAX, native))
                        return CommandStatus::invalid;
                    if (!owner.set_coefficient(phase, field, std::uint32_t(native)))
                        return CommandStatus::failed;
                    return owner.write_primary_calibration(output, field) ? CommandStatus::handled
                                                                          : CommandStatus::failed;
                }
                if (!support::parse_unsigned_decimal(
                        request.payload, owner.maximum_coefficient(field), value))
                    return CommandStatus::invalid;
                // The pinned common core treats 2..99 as a query; 1 restores the ADE default.
                if ((value == 1 || value > 99) && !owner.set_coefficient(phase, field, value))
                    return CommandStatus::failed;
            }
        }
        return owner.write_primary_calibration(output, field) ? CommandStatus::handled
                                                              : CommandStatus::failed;
    }
    if (request.has_index)
        return CommandStatus::invalid;
    if (module_address || range_config) {
        if (!request.is_query()) {
            auto value = request.payload;
            if (range_config)
                value.remove_prefix(6);
            std::uint32_t parsed{};
            if (!support::parse_unsigned_decimal(value, module_address ? 3U : 300U, parsed) ||
                !parsed)
                return CommandStatus::invalid;
            if (!owner.control(
                    module_address ? Control::module_address : Control::current_range, parsed))
                return CommandStatus::failed;
        }
        (void)owner.write_control(output);
        return CommandStatus::handled;
    }
#if USE_ENERGY_REGISTERS
    if (!request.is_query() && owner.configure_registers(request.payload))
        return owner.write_registers(output) ? CommandStatus::handled : CommandStatus::failed;
#endif
    return CommandStatus::invalid;
}
} // namespace newmota::energy
