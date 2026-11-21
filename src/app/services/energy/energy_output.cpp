/*
  energy_output.cpp - Primary ENERGY accounting telemetry projection

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Output reference: Tasmota xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later. Independent
  exact composition; no upstream floating settings layout is copied.
*/

/** @file energy_output.cpp
 * @brief Read-only primary accounting and measured-power output shared by StatusSNS and MQTT
 * SENSOR.
 */
#include "services/energy/energy_service.h"
#include "core/command/command.h"
#include "services/time/time_service.h"
#include "services/energy/energy_sum.h"
#include "drivers/sensors/sensor_service.h"
#include <cmath>
#include <cstdio>

namespace newmota::energy {
namespace {
/** @brief Emits phase arrays or one exact sum. As in Tasmota, an unknown phase reads as zero. */
template <class Read>
bool phases(CommandOutput& output, std::size_t count, bool per_phase, unsigned places, Read read) {
    if (per_phase && count > 1) {
        if (!output.write("["))
            return false;
        for (std::size_t c = 0; c < count; ++c) {
            SignedEnergy value{};
            detail::EnergySum sum{};
            if (read(c, value))
                sum.add(value);
            if ((c && !output.write(",")) || !sum.write(output, places))
                return false;
        }
        return output.write("]");
    }
    detail::EnergySum sum{};
    for (std::size_t c = 0; c < count; ++c) {
        SignedEnergy value{};
        if (read(c, value))
            sum.add(value);
    }
    return sum.write(output, places);
}

/** @brief Source-compatible AC display values; invalid reactive math is explicitly unknown. */
struct AcProjection {
    std::int64_t apparent{}, reactive{}, factor{};
    bool reactive_valid{true};
};

/** @brief Converts a finite display calculation to milli-units without overflowing a cast.
 * @param value Calculated milli-unit value.
 * @param result Receives the bounded integer on success.
 * @return false for non-finite or out-of-int64 values.
 */
bool bounded_milli(long double value, std::int64_t& result) {
    if (!std::isfinite(value) || value < -9223372036854775808.0L || value >= 9223372036854775808.0L)
        return false;
    result = static_cast<std::int64_t>(value);
    return true;
}

/** @brief Independent floor square root for the source's bounded uint32 reactive domain. */
std::uint32_t floor_root(std::uint32_t input) {
    std::uint32_t low = 0, high = 65536;
    while (low + 1 < high) {
        const auto middle = low + (high - low) / 2;
        if (std::uint64_t(middle) * middle <= input)
            low = middle;
        else
            high = middle;
    }
    return low;
}

/** @brief Derives the pinned EnergyShow AC quantities without changing samples/accounts.
 * Native typed values take precedence. Source positive-active/apparent correction and
 * current/difference thresholds are retained. Negative or out-of-uint32 sqrt domains are
 * unknown instead of reproducing undefined source float-to-unsigned conversions.
 */
bool project_ac(const Sample& sample, const Sample& voltage, AcProjection& result) {
    if (!sample.presentation_valid() || !voltage.presentation_valid() || sample.direct_current ||
        (sample.available_fields & 6) != 6 || !(voltage.available_fields & 1))
        return false;
    const auto power = sample.current ? sample.power : std::int64_t(0);
    if (sample.auxiliary_fields & (1U << 4))
        result.apparent = sample.current ? sample.apparent_mva : 0;
    else if (!bounded_milli(static_cast<long double>(voltage.voltage) * sample.current / 1000,
                 result.apparent))
        return false;
    if (sample.auxiliary_fields & 2)
        result.factor = sample.power_factor_milli;
    else if (!bounded_milli(power && result.apparent
                                ? static_cast<long double>(power) * 1000 / result.apparent
                                : 0,
                 result.factor))
        return false;
    if (result.apparent < power) {
        result.apparent = power;
        if (result.factor > 1000)
            result.factor = 1000;
    }
    if (sample.auxiliary_fields & (1U << 5)) {
        result.reactive = sample.current ? sample.reactive_mvar : 0;
        return true;
    }
    const auto apparent = static_cast<long double>(result.apparent);
    const auto active = static_cast<long double>(power);
    const auto square = (apparent * apparent - active * active) / 1000000;
    if (!std::isfinite(square) || result.apparent < 0 || square < 0) {
        result.reactive_valid = false;
        return true;
    }
    const auto difference = std::floor((std::trunc(apparent / 10) - std::trunc(active / 10)) / 10);
    if (sample.current > 5 && (difference > 15 || difference > std::trunc(apparent / 10000))) {
        if (square < 10737418) {
            const auto root = floor_root(static_cast<std::uint32_t>(square * 100) * 4);
            result.reactive = std::int64_t((root + 1) / 2) * 100;
        } else if (square <= UINT32_MAX)
            result.reactive = std::int64_t(floor_root(static_cast<std::uint32_t>(square))) * 1000;
        else
            result.reactive_valid = false;
    }
    return true;
}

/** @brief Source active power in mW; AC output zeroes it for an available zero-current reading. */
bool measured_power(const Sample& sample, std::int64_t& power) {
    if (!sample.presentation_valid() || !(sample.available_fields & 4))
        return false;
    power = !sample.direct_current && (sample.available_fields & 3) == 3 && !sample.current
                ? std::int64_t(0)
                : sample.power;
    return true;
}

std::int32_t saturate(std::int64_t value) {
    return value > INT32_MAX ? INT32_MAX : value < INT32_MIN ? INT32_MIN : std::int32_t(value);
}

/** @brief Rounds exact energy to the milli-kWh steps of a sensor reading. */
std::int32_t milli_kwh(const SignedEnergy& value) {
    const auto wh = std::min<std::uint64_t>(
        value.magnitude.wh + (value.magnitude.micro_wh >= 500000 ? 1 : 0), INT32_MAX);
    return value.negative ? -std::int32_t(wh) : std::int32_t(wh);
}
} // namespace

bool Service::write_energy_object(CommandOutput& output, bool period) const {
    (void)period;
    const auto& instance = provider_;
    const auto count = instance.configuration.channels;
    if (!count || !instance.channels)
        return false;
    const bool per_phase = phase_presentation_ && phase_presentation_(phase_context_);
    const auto time = TimeService::civil_time(instance.total_time);
    if (!output.format("\"ENERGY\":{\"TotalStartTime\":\"%04u-%02u-%02uT%02u:%02u:%02u\"",
            unsigned(time.year), unsigned(time.month), unsigned(time.day), unsigned(time.hour),
            unsigned(time.minute), unsigned(time.second)))
        return false;
    if (!output.write(",\"Total\":") ||
        !phases(output, count, per_phase, output_resolution(OutputQuantity::energy),
            [&](std::size_t c, SignedEnergy& value) {
                return instance.channels[c].account.compatibility_total(value);
            }))
        return false;
#if USE_ENERGY_TARIFF_COMPAT
    if (!write_tariff_fields(output, false))
        return false;
#endif
    for (const bool previous : {true, false}) {
        if (!output.format(",\"%s\":", previous ? "Yesterday" : "Today") ||
            !phases(output, count, per_phase, output_resolution(OutputQuantity::energy),
                [&](std::size_t c, SignedEnergy& value) {
                    const auto& channel = instance.channels[c];
                    if (!previous) {
                        if (!channel.live_daily.present || channel.live_daily.fault ||
                            channel.live_daily.incomplete)
                            return false;
                        value = channel.live_daily.value;
                        return true;
                    }
                    if (!channel.yesterday_present)
                        return false;
                    value = channel.yesterday;
                    return true;
                }))
            return false;
    }
    const bool reporting = export_reporting();
#if USE_ENERGY_BALANCED_DAILY
    if (reporting ||
        (instance.channels[0].sample.valid && (instance.channels[0].sample.counter_fields & 2)))
        for (unsigned direction = 0; direction < 2; ++direction) {
            const auto* balanced = !instance.balanced_fault ? instance.balanced.get() : nullptr;
            if (!output.format(",\"%s\":", direction ? "TodaySumExport" : "TodaySumImport") ||
                !phases(output, 1, false, output_resolution(OutputQuantity::energy),
                    [&](std::size_t, SignedEnergy& value) {
                        if (!balanced)
                            return false;
                        value = {
                            direction ? balanced->runtime.exported : balanced->runtime.imported,
                            false};
                        return true;
                    }))
                return false;
        }
#endif
    if (reporting) {
        const auto read = [&](std::size_t c, SignedEnergy& value) {
            return instance.channels[c].account.compatibility_export(value);
        };
        if (!output.write(",\"ExportActive\":") ||
            !phases(output, count, true, output_resolution(OutputQuantity::energy), read))
            return false;
        if (count > 1 && !per_phase &&
            (!output.write(",\"ExportActiveTotal\":") ||
                !phases(output, count, false, output_resolution(OutputQuantity::energy), read)))
            return false;
#if USE_ENERGY_TARIFF_COMPAT
        if (!write_tariff_fields(output, true))
            return false;
#endif
    }
#if USE_ENERGY_PERIOD_COMPAT
    if (period && (!output.write(",\"Period\":") || !write_period(output)))
        return false;
#endif
    /** The same normalized magnitude writer divides by 1000. For this read-only projection
     * its whole-unit limb represents mW, producing W; no energy account is touched.
     * Source AC output zeroes active power for an available zero-current V/I reading.
     */
    const auto read_power = [&](std::size_t c, SignedEnergy& value) {
        std::int64_t power{};
        if (!measured_power(instance.channels[c].sample, power))
            return false;
        const auto magnitude = power < 0 ? std::uint64_t(-(power + 1)) + 1 : std::uint64_t(power);
        value = {EnergyValue{magnitude, 0}, power < 0};
        return true;
    };
    if (!output.write(",\"Power\":") ||
        !phases(output, count, true, output_resolution(OutputQuantity::power), read_power))
        return false;
    if (count > 1 && !per_phase &&
        (!output.write(",\"PowerTotal\":") ||
            !phases(output, count, false, output_resolution(OutputQuantity::power), read_power)))
        return false;
    const bool no_common = no_common_presentation_ && no_common_presentation_(no_common_context_);
    const bool common_voltage = !no_common && instance.device && instance.device->common_voltage();
    bool show_ac = false;
    for (std::size_t c = 0; c < count; ++c) {
        const auto& sample = instance.channels[c].sample;
        const auto& voltage = instance.channels[common_voltage ? 0 : c].sample;
        show_ac |= !sample.direct_current && (sample.available_fields & 2) &&
                   (voltage.available_fields & 1);
    }
    if (show_ac)
        for (unsigned field = 0; field < 3; ++field) {
            if (!output.format(",\"%s\":", field == 0   ? "ApparentPower"
                                           : field == 1 ? "ReactivePower"
                                                        : "Factor") ||
                !phases(output, count, true,
                    output_resolution(field == 2 ? OutputQuantity::factor : OutputQuantity::power),
                    [&](std::size_t c, SignedEnergy& value) {
                        AcProjection projection{};
                        if (!project_ac(instance.channels[c].sample,
                                instance.channels[common_voltage ? 0 : c].sample, projection) ||
                            (field == 1 && !projection.reactive_valid))
                            return false;
                        const auto raw = field == 0   ? projection.apparent
                                         : field == 1 ? projection.reactive
                                                      : projection.factor;
                        const auto magnitude =
                            raw < 0 ? std::uint64_t(-(raw + 1)) + 1 : std::uint64_t(raw);
                        value = {EnergyValue{magnitude, 0}, raw < 0};
                        return true;
                    }))
                return false;
        }
    for (const unsigned field : {2U, 0U, 1U}) {
        const auto mask = field == 1 ? 2U : 1U;
        const auto present = [&](const Sample& sample) {
            return field == 2 ? (sample.auxiliary_fields & 1U) != 0
                              : (sample.available_fields & mask) != 0;
        };
        // Pinned EnergyShow gates frequency on phase zero before choosing common/array format.
        bool available = field == 2 && present(instance.channels[0].sample);
        if (field != 2)
            for (std::size_t c = 0; c < count; ++c)
                available |= present(instance.channels[c].sample);
        if (!available)
            continue;
        const bool common = !no_common && instance.device &&
                            (field == 0 ? instance.device->common_voltage()
                                        : field == 2 && instance.device->common_frequency());
        if (!output.format(",\"%s\":", field == 0   ? "Voltage"
                                       : field == 1 ? "Current"
                                                    : "Frequency") ||
            !phases(output, common ? 1 : count, true,
                output_resolution(field == 0   ? OutputQuantity::voltage
                                  : field == 1 ? OutputQuantity::current
                                               : OutputQuantity::frequency),
                [&](std::size_t c, SignedEnergy& value) {
                    const auto& sample = instance.channels[c].sample;
                    if (!sample.presentation_valid() || !present(sample))
                        return false;
                    const auto raw = field == 0   ? sample.voltage
                                     : field == 1 ? sample.current
                                                  : std::int64_t(sample.frequency_millihz);
                    const auto magnitude =
                        raw < 0 ? std::uint64_t(-(raw + 1)) + 1 : std::uint64_t(raw);
                    value = {EnergyValue{magnitude, 0}, raw < 0};
                    return true;
                }))
            return false;
    }
#if USE_ADE7880
    // Tasmota's ADE7880 appends the shared neutral current after the common fields.
    if (const auto& first = instance.channels[0].sample;
        (first.auxiliary_fields & (1U << 15)) &&
        (!output.write(",\"CurrentNeutral\":") ||
            !phases(output, 1, false, output_resolution(OutputQuantity::current),
                [&](std::size_t, SignedEnergy& value) {
                    if (!first.presentation_valid())
                        return false;
                    const auto raw = std::int64_t(first.neutral_current_ma);
                    const auto magnitude =
                        raw < 0 ? std::uint64_t(-(raw + 1)) + 1 : std::uint64_t(raw);
                    value = {EnergyValue{magnitude, 0}, raw < 0};
                    return true;
                })))
        return false;
#endif
#if NEWMOTA_ENERGY_JSON_FIELDS
    if (instance.device && !instance.device->write_json_fields(output))
        return false;
#endif
    return output.write("}");
}

bool Service::visit_sensor_readings(
    bool (*visitor)(void*, const SensorReading&), void* context) const {
    const auto& instance = provider_;
    const auto count = instance.configuration.channels;
    if (!instance.configuration.identity.driver || !count || !instance.channels)
        return true;
    const bool numbered = count > 1;
    const bool per_phase = phase_presentation_ && phase_presentation_(phase_context_);
    const bool no_common = no_common_presentation_ && no_common_presentation_(no_common_context_);
    const bool common_voltage = !no_common && instance.device && instance.device->common_voltage();
    const bool common_frequency =
        !no_common && instance.device && instance.device->common_frequency();
    bool ok = true;
    // One card for all phases: a field of a multi-phase meter carries its phase number.
    const auto emit = [&](const char* field, SensorQuantity quantity, std::int64_t value,
                          std::size_t phase, bool phased, bool history = false,
                          const char* unit = nullptr, std::uint8_t decimals = 0) {
        char name[40];
        if (phased)
            std::snprintf(name, sizeof(name), "%s%u", field, unsigned(phase + 1));
        SensorReading reading{"ENERGY", phased ? name : field, quantity, saturate(value)};
        reading.dashboard_only = true;
        reading.no_history = !history;
        reading.unit = unit;
        reading.decimals = decimals;
        ok = ok && visitor(context, reading);
    };
    const auto& first = instance.channels[0].sample;
    for (std::size_t c = 0; c < (common_voltage ? 1 : count); ++c) {
        const auto& sample = instance.channels[c].sample;
        if (sample.presentation_valid() && (sample.available_fields & 1))
            emit(
                "Voltage", SensorQuantity::voltage, sample.voltage, c, numbered && !common_voltage);
    }
    if (first.auxiliary_fields & 1)
        for (std::size_t c = 0; c < (common_frequency ? 1 : count); ++c) {
            const auto& sample = instance.channels[c].sample;
            if (sample.presentation_valid() && (sample.auxiliary_fields & 1))
                emit("Frequency", SensorQuantity::frequency, sample.frequency_millihz, c,
                    numbered && !common_frequency);
        }
    for (std::size_t c = 0; c < count; ++c) {
        const auto& sample = instance.channels[c].sample;
        if (sample.presentation_valid() && (sample.available_fields & 2))
            emit("Current", SensorQuantity::current, sample.current, c, numbered);
    }
    std::int64_t total{};
    bool total_valid = true;
    for (std::size_t c = 0; c < count; ++c) {
        std::int64_t power{};
        if (!measured_power(instance.channels[c].sample, power)) {
            total_valid = false;
            continue;
        }
        emit("Power", SensorQuantity::power, power, c, numbered, true);
        total += power;
    }
    if (numbered && total_valid)
        emit("PowerTotal", SensorQuantity::power, total, 0, false, true);
    for (std::size_t c = 0; c < count; ++c) {
        AcProjection projection{};
        if (!project_ac(instance.channels[c].sample,
                instance.channels[common_voltage ? 0 : c].sample, projection))
            continue;
        emit("ApparentPower", SensorQuantity::apparent_power, projection.apparent, c, numbered);
        if (projection.reactive_valid)
            emit("ReactivePower", SensorQuantity::reactive_power, projection.reactive, c, numbered);
        emit("Factor", SensorQuantity::power_factor, projection.factor, c, numbered);
    }
    // As in Tasmota's table, the daily and lifetime totals are sums unless SetOption129 is on.
    const auto energy = [&](const char* field, const auto& read, bool phased) {
        if (phased && numbered) {
            for (std::size_t c = 0; c < count; ++c) {
                SignedEnergy value{};
                if (read(instance.channels[c], value))
                    emit(field, SensorQuantity::energy, milli_kwh(value), c, true);
            }
            return;
        }
        detail::EnergySum sum{};
        for (std::size_t c = 0; c < count; ++c) {
            SignedEnergy value{};
            if (!read(instance.channels[c], value))
                return;
            sum.add(value);
        }
        SignedEnergy value{};
        if (sum.value(value))
            emit(field, SensorQuantity::energy, milli_kwh(value), 0, false);
    };
    energy(
        "Today",
        [](const Channel& channel, SignedEnergy& value) {
            if (!channel.live_daily.present || channel.live_daily.fault ||
                channel.live_daily.incomplete)
                return false;
            value = channel.live_daily.value;
            return true;
        },
        per_phase);
    energy(
        "Yesterday",
        [](const Channel& channel, SignedEnergy& value) {
            if (!channel.yesterday_present)
                return false;
            value = channel.yesterday;
            return true;
        },
        per_phase);
    energy(
        "Total",
        [](const Channel& channel, SignedEnergy& value) {
            return channel.account.compatibility_total(value);
        },
        per_phase);
    if (export_reporting())
        energy(
            "ExportActive",
            [](const Channel& channel, SignedEnergy& value) {
                return channel.account.compatibility_export(value);
            },
            true);
#if USE_ADE7880
    if (first.presentation_valid() && (first.auxiliary_fields & (1U << 15)))
        emit("CurrentNeutral", SensorQuantity::current, first.neutral_current_ma, 0, false);
#endif

#if USE_MODBUS_ENERGY
    // Driver rows follow, each in its own unit.
    struct Rows {
        const decltype(emit)& write;
        bool numbered;
    } rows{emit, numbered};

    if (ok && instance.device)
        (void)instance.device->visit_user_measurements(
            [](void* opaque, const UserMeasurement& measured) {
                const auto& rows = *static_cast<const Rows*>(opaque);
                const auto micro = measured.micro;
                const auto magnitude =
                    micro < 0 ? std::uint64_t(-(micro + 1)) + 1 : std::uint64_t(micro);
                const auto rounded =
                    std::int64_t(std::min<std::uint64_t>((magnitude + 500) / 1000, INT32_MAX));
                const auto milli = micro < 0 ? -rounded : rounded;
                rows.write(measured.name, SensorQuantity::other, milli, measured.phase,
                    rows.numbered, false, measured.unit, measured.decimals);
                return true;
            },
            &rows);
#endif
#if NEWMOTA_ENERGY_CHIP_TEMPERATURE
    std::int32_t temperature{};
    if (instance.device && instance.device->chip_temperature(temperature))
        emit("Temperature", SensorQuantity::temperature, temperature, 0, false);
#endif
    return ok;
}

#if USE_MATTER_ENERGY
bool Service::matter_reading(MatterReading& output) const {
    const auto& instance = provider_;
    // Meters with more channels wait for hardware to decide how they map onto endpoints.
    if (!instance.configuration.identity.driver || instance.configuration.channels != 1 ||
        !instance.channels)
        return false;
    output = {};
    const auto& sample = instance.channels[0].sample;
    const auto& account = instance.channels[0].account;
    const auto mwh = [](const EnergyValue& value) {
        return value.wh * 1000 + value.micro_wh / 1000;
    };
    output.imported_mwh = mwh(account.imported);
    output.exported_mwh = mwh(account.exported);
    output.direct_current = sample.direct_current;
    output.exports = export_reporting() || (sample.valid && (sample.counter_fields & 2)) ||
                     output.exported_mwh != 0;
    if (!sample.presentation_valid())
        return true;
    if (sample.available_fields & 1) {
        output.voltage = sample.voltage;
        output.present |= MatterReading::voltage_bit;
    }
    if (sample.available_fields & 2) {
        output.current = sample.current;
        output.present |= MatterReading::current_bit;
    }
    if (measured_power(sample, output.power))
        output.present |= MatterReading::power_bit;
    if (sample.auxiliary_fields & 1U) {
        output.frequency = sample.frequency_millihz;
        output.present |= MatterReading::frequency_bit;
    }
    AcProjection projection{};
    if (project_ac(sample, sample, projection)) {
        output.apparent = projection.apparent;
        output.factor = projection.factor * 10;
        output.present |= MatterReading::apparent_bit | MatterReading::factor_bit;
        if (projection.reactive_valid) {
            output.reactive = projection.reactive;
            output.present |= MatterReading::reactive_bit;
        }
    }
    return true;
}
#endif
} // namespace newmota::energy
