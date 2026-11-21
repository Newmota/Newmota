/*
  energy_module.cpp - Safe Berry energy instance and phase bindings

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  API names/units adapted from Tasmota xdrv_52_3_berry_energy.ino,
  be_energylib.c and embedded/energy.be.
  Copyright (C) 2021 Stephan Hadinger; Berry language by Guan Wenliang.
  Upstream distributed under GPL version 3 or later, without any warranty.
  Daily, Total, Period and export projection reference: Tasmota xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later.
*/

/** @file energy_module.cpp
 * @brief Copies typed values through the energy owner; scripts never receive native addresses.
 */
#include "services/berry/modules/energy/energy_module.h"
#include "services/energy/energy_service.h"
#include "newmota/platform/platform.h"
#include "support/number.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>
extern "C" {
#include "berry.h"
#include "be_gc.h"
#include "be_module.h"
#include "be_string.h"
}

namespace {
using namespace newmota::energy;

int integer(bvm* vm, int index, int low, int high) {
    if (index > be_top(vm) || !be_isint(vm, index))
        be_raise(vm, "type_error", "expected an integer");
    const auto value = be_toint(vm, index);
    if (value < low || value > high)
        be_raise(vm, "value_error", "energy index out of range");
    return static_cast<int>(value);
}

std::string_view text(bvm* vm, int index) {
    if (index > be_top(vm) || !be_isstring(vm, index))
        be_raise(vm, "type_error", "expected an energy field string");
    return {be_tostring(vm, index), static_cast<std::size_t>(be_strlen(vm, index))};
}

long double number(bvm* vm, int index) {
    if (index > be_top(vm) ||
        (!be_isint(vm, index) && !be_isreal(vm, index) && !be_isbool(vm, index)))
        be_raise(vm, "type_error", "expected a finite number");
    const long double value = be_isbool(vm, index) ? be_tobool(vm, index)
                              : be_isint(vm, index)
                                  ? static_cast<long double>(be_toint(vm, index))
                                  : static_cast<long double>(be_toreal(vm, index));
    if (!std::isfinite(value))
        be_raise(vm, "value_error", "nonfinite energy value");
    return value;
}

/** @brief Validates a copied channel without exposing retained storage.
 * @param observe Normal measurement access polls first; interval access passes false so reading
 * alone cannot collect a delta, synchronize publication state or change a prepared token.
 * @return A value copy belonging to the checked configured identity.
 */
Snapshot snapshot(bvm* vm, int channel, std::uint32_t generation = 0, bool observe = true) {
    if (observe)
        service().poll(newmota::platform::monotonic_milliseconds());
    Snapshot value{};
    if (!service().snapshot(channel, value) || (generation && value.generation != generation))
        be_raise(vm, "value_error", "absent energy channel or stale provider");
    return value;
}

void writable(bvm* vm, const Snapshot& value) {
    if (!value.writable || value.driver != 27)
        be_raise(vm, "value_error", "hardware energy providers are read-only");
}

void put_integer(bvm* vm, const char* key, bint value) {
    be_pushstring(vm, key);
    be_pushint(vm, value);
    (void)be_data_insert(vm, -3);
    be_pop(vm, 2);
}

void put_wh(bvm* vm, const char* key, std::uint64_t value) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
    be_pushstring(vm, key);
    be_pushstring(vm, buffer);
    (void)be_data_insert(vm, -3);
    be_pop(vm, 2);
}

long double kwh(EnergyValue value) {
    return static_cast<long double>(value.wh) / 1000 +
           static_cast<long double>(value.micro_wh) / 1000000000;
}

/** @brief Converts a copied exact signed projection to Berry kWh without changing accounts. */
long double kwh(const SignedEnergy& value) {
    return kwh(value.magnitude) * (value.negative ? -1 : 1);
}

EnergyValue from_kwh(bvm* vm, long double value) {
    const auto wh = value * 1000;
    if (wh < 0 || wh >= static_cast<long double>(UINT64_MAX))
        be_raise(vm, "value_error", "energy total out of range; use exact Wh API");
    auto whole = static_cast<std::uint64_t>(wh);
    auto fraction = static_cast<std::uint32_t>((wh - whole) * 1000000);
    if (fraction >= 1000000) {
        ++whole;
        fraction = 0;
    }
    return {whole, fraction};
}

EnergyValue exact(bvm* vm, int whole_index, int fraction_index) {
    std::uint64_t whole{};
    if (be_isstring(vm, whole_index)) {
        const auto input = text(vm, whole_index);
        if (input.empty() || input.size() > 20)
            be_raise(vm, "value_error", "invalid whole-Wh decimal string");
        for (const char c : input) {
            if (c < '0' || c > '9' || whole > (UINT64_MAX - unsigned(c - '0')) / 10)
                be_raise(vm, "value_error", "whole-Wh overflow");
            whole = whole * 10 + unsigned(c - '0');
        }
    } else {
        const auto value = integer(vm, whole_index, 0, INT32_MAX);
        whole = static_cast<std::uint64_t>(value);
    }
    return {whole, std::uint32_t(integer(vm, fraction_index, 0, 999999))};
}

int Info(bvm* vm) {
    Snapshot current{};
    if (!service().snapshot(1, current))
        be_return_nil(vm);
    be_newobject(vm, "map");
    put_integer(vm, "generation", current.generation);
    put_integer(vm, "phase_count", current.channels);
    put_integer(vm, "driver_id", current.driver);
    // be_newobject leaves instance + .p; return the instance, not its raw backing map.
    be_pop(vm, 1);
    be_return(vm);
}

/** @brief Resolves scalar suffixes through the bounded service channel limit.
 * Configured-count and generation checks still occur at snapshot access. This also
 * lets copied read maps address hardware meters wider than the virtual provider's eight phases.
 */
void field_channel(std::string_view& field, int& channel) {
    const auto separator = field.rfind('_');
    std::uint32_t selected{};
    if (separator != std::string_view::npos &&
        newmota::support::parse_unsigned_decimal(
            field.substr(separator + 1), Service::kMaximumChannels, selected) &&
        selected >= 2) {
        channel = static_cast<int>(selected);
        field = field.substr(0, separator);
    }
}

int Read(bvm* vm) {
    auto field = text(vm, 1);
    int channel = integer(vm, 2, 1, Service::kMaximumChannels);
    const auto requested_field = field;
    field_channel(field, channel);
    const bool topology = field == "voltage_common" || field == "frequency_common";
    const bool sample_metadata =
        field == "type_dc" || field == "voltage_available" || field == "current_available";
    /** @brief Model/sample policy metadata is meter-wide, never a suffixed phase measurement. */
    if ((topology || sample_metadata) && (field != requested_field || channel != 1))
        be_raise(vm, "attribute_error", "meter metadata is scalar");
    bool observe = !topology && field != "use_overtemp" && field != "power_on";
#if USE_BERRY_ENERGY_PERIOD_API
    observe = observe && field != "period";
#endif
    const auto generation = integer(vm, 3, 1, INT32_MAX);
    /** @brief First check runtime ownership without polling. A failed-open retained identity
     * cannot supply sample metadata; known owners then use the normal measurement poll.
     */
    const bool age_read = field == "data_valid";
    auto current = snapshot(vm, channel, generation, sample_metadata || age_read ? false : observe);
    if ((sample_metadata && current.sample_metadata_available) ||
        (age_read && current.writable && !current.observation_age_available))
        current = snapshot(vm, channel, generation);
    const auto& sample = current.sample;
    long double value{};
    if (sample_metadata) {
        /** @brief Virtual policy remains exactly its durable option mask. Hardware reads only
         * copied current samples: channel-1 AC/DC type, and any configured channel's V/I bits.
         */
        if (current.writable) {
            const auto mask = field == "type_dc" ? 4U : field == "voltage_available" ? 8U : 16U;
            be_pushbool(vm, (current.options.flags & mask) != 0);
        } else {
            if (!current.sample_metadata_available)
                be_return_nil(vm);
            be_pushbool(vm, field == "type_dc"             ? current.direct_current
                            : field == "voltage_available" ? current.voltage_available
                                                           : current.current_available);
        }
        be_return(vm);
    } else if (topology) {
        /** @brief Virtual options and hardware topology are distinct contracts. Retain the
         * provider's stored flags; never substitute its source-model Device overrides.
         */
        if (current.writable) {
            const auto mask = field == "voltage_common" ? 1U : 2U;
            be_pushbool(vm, (current.options.flags & mask) != 0);
        } else {
            if (!current.topology_available)
                be_return_nil(vm);
            be_pushbool(
                vm, field == "voltage_common" ? current.common_voltage : current.common_frequency);
        }
        be_return(vm);
    } else if (field == "power_on") {
        /** @brief Primary uses actual logical Power OR SO21; the override never sets an output
         * bit for the selected provider. This read does not poll.
         */
        be_pushbool(vm, current.power_on);
        be_return(vm);
    } else if (field == "use_overtemp") {
        /** @brief This is the selected model's policy request, not active enforcement,
         * a temperature measurement, or permission to change the shared shutdown policy.
         */
        if (!current.overtemperature_available)
            be_return_nil(vm);
        be_pushbool(vm, current.overtemperature_requested);
        be_return(vm);
    } else if (field == "voltage")
        value = sample.voltage / 1000.0L;
    else if (field == "current")
        value = sample.current / 1000.0L;
    else if (field == "active_power")
        value = sample.power / 1000.0L;
    else if (field == "apparent_power")
        value = sample.apparent_mva / 1000.0L;
    else if (field == "reactive_power")
        value = sample.reactive_mvar / 1000.0L;
    else if (field == "power_factor")
        value = sample.power_factor_milli / 1000.0L;
    else if (field == "frequency")
        value = sample.frequency_millihz / 1000.0L;
    else if (field == "import_active")
        value = kwh(sample.import_counter);
    else if (field == "export_active" && service().export_reporting()) {
        /** @brief Source local reporting replaces the public array with retained export energy;
         * native meter counters remain separate and an unrepresentable projection is unknown.
         */
        SignedEnergy projection{};
        if (!current.account.compatibility_export(projection))
            be_return_nil(vm);
        value = kwh(projection);
    } else if (field == "export_active" || field == "export_counter") {
        /** @brief Counter events can be consumed before this read; retain the provider's raw
         * value rather than treating its once-only event mask as measurement availability.
         */
        value = kwh(sample.export_counter);
    } else if (field == "total") {
        /** @brief Source total is a recomputed compatibility projection, not the physical
         * imported lifetime account or the provider's native import_active counter.
         */
        SignedEnergy projection{};
        if (!current.account.compatibility_total(projection))
            be_return_nil(vm);
        value = kwh(projection);
    } else if (field == "import_total")
        value = kwh(current.account.imported);
    else if (field == "export_total")
        value = kwh(current.account.exported);
    else if (field == "start_energy")
        value = kwh(current.account.import_baseline);
    else if (field == "temperature")
        value = sample.temperature_millicelsius / 1000.0L;
    else if (field == "battery_voltage")
        value = sample.battery_voltage_mv / 1000.0L;
    else if (field == "battery_current")
        value = sample.battery_current_ma / 1000.0L;
#if USE_BERRY_ENERGY_PERIOD_API
    else if (field == "period") {
        /** @brief This checked API exposes pending signed energy in kWh, not Tasmota's
         * writable daily baseline or the prepared SENSOR Wh preview. Neither is consumed.
         */
        if (!current.pending_period_available)
            be_return_nil(vm);
        value = kwh(current.pending_period);
    }
#endif
#if USE_BERRY_ENERGY_ACCOUNTING_API
    else if (field == "daily") {
        const auto& live = current.live_daily;
        if (!live.present || live.incomplete || live.fault)
            be_return_nil(vm);
        value = kwh(live.value);
    } else if (field == "daily_sum" || field == "yesterday_sum") {
        /** @brief Sum only this identity's channels; an unknown phase makes the sum unknown.
         * Dated Calendar, lifetime counters and arbitrary meters never substitute for a day.
         */
        for (unsigned c = 1; c <= current.channels; ++c) {
            auto next = current;
            /** @brief The initial read already polled; copy remaining phases without another
             * protocol/accounting update between members of this one-meter projection.
             */
            if (c != unsigned(channel) &&
                (!service().snapshot(c, next) || next.generation != current.generation))
                be_raise(vm, "value_error", "absent energy channel or stale instance");
            if (field == "daily_sum") {
                if (!next.live_daily.present || next.live_daily.incomplete || next.live_daily.fault)
                    be_return_nil(vm);
                value += kwh(next.live_daily.value);
            } else {
                if (!next.yesterday_present)
                    be_return_nil(vm);
                value += kwh(next.yesterday);
            }
        }
    }
#endif
#if USE_BERRY_ENERGY_BALANCED_API
    else if (field == "daily_sum_import_balanced" || field == "daily_sum_export_balanced") {
        /** @brief The owner already balances its phase-update groups; never sum phase daily
         * values, derive a lifetime difference or require a trusted date in this read path.
         */
        if (!current.balanced_daily_available)
            be_return_nil(vm);
        value = kwh(field == "daily_sum_import_balanced" ? current.balanced_daily.imported
                                                         : current.balanced_daily.exported);
    }
#endif
    else if (field == "energy_active_export") {
        /** @brief Source local_energy_active_export is meter runtime reporting state,
         * not a persisted virtual-provider counter direction option.
         */
        be_pushbool(vm, service().export_reporting());
        be_return(vm);
    } else if (field == "total_sum" || field == "import_total_sum" || field == "export_total_sum") {
        /** @brief Sum one copied meter observation in exact units. Further channel copies
         * must not poll devices again or blend observations across accounting boundaries.
         */
        SignedEnergy sum{};
        for (unsigned c = 1; c <= current.channels; ++c) {
            auto next = current;
            if (c != unsigned(channel) &&
                (!service().snapshot(c, next) || next.generation != current.generation))
                be_raise(vm, "value_error", "absent energy channel or stale instance");
            SignedEnergy phase{};
            if (field == "total_sum") {
                if (!next.account.compatibility_total(phase))
                    be_return_nil(vm);
            } else {
                phase.magnitude =
                    field == "import_total_sum" ? next.account.imported : next.account.exported;
            }
            if (!sum.add(phase.magnitude, phase.negative))
                be_return_nil(vm);
        }
        value = kwh(sum);
    } else if (field == "phase_count") {
        be_pushint(vm, current.channels);
        be_return(vm);
    } else if (field == "data_valid") {
        /** @brief Virtual injection retains its established 0/255 marker. Hardware must supply
         * an actual source age owner; validity, transport and value changes cannot invent one.
         */
        if (current.observation_age_available)
            be_pushint(vm, current.observation_age);
        else if (current.writable)
            be_pushint(vm, sample.valid ? 0 : 255);
        else
            be_return_nil(vm);
        be_return(vm);
    } else if (field == "driver_id") {
        be_pushint(vm, current.driver);
        be_return(vm);
    } else {
        unsigned mask{};
        if (field == "counter_mode")
            mask = 32;
        else if (field == "counter_export_routing")
            mask = 64;
        else if (field != "valid")
            be_raise(vm, "attribute_error", "energy field has no implemented service mapping");
        if (!current.writable && mask)
            be_raise(
                vm, "attribute_error", "provider policy fields are not hardware driver options");
        be_pushbool(vm, mask ? (current.options.flags & mask) != 0 : sample.valid);
        be_return(vm);
    }
    be_pushreal(vm, static_cast<breal>(value));
    be_return(vm);
}

int Write(bvm* vm) {
    auto field = text(vm, 1);
    int channel = integer(vm, 2, 1, 8);
    const auto requested_field = field;
    field_channel(field, channel);
    if ((field == "voltage_common" || field == "frequency_common" || field == "type_dc" ||
            field == "voltage_available" || field == "current_available") &&
        (field != requested_field || channel != 1))
        be_raise(vm, "attribute_error", "meter metadata is scalar");
    auto current = snapshot(
        vm, channel, integer(vm, 3, 1, INT32_MAX), field != "use_overtemp" && field != "power_on");
    /** @brief Upstream permits raw writes; the checked API intentionally keeps policy metadata
     * read-only and cannot toggle protection or persist a request override.
     */
    if (field == "power_on")
        be_raise(vm, "attribute_error", "shared power policy is read-only");
    if (field == "use_overtemp")
        be_raise(vm, "attribute_error", "overtemperature policy metadata is read-only");
#if USE_BERRY_ENERGY_PERIOD_API
    /** @brief Period is an owner snapshot, never a measurement injection or publish-state write. */
    if (field == "period")
        be_raise(vm, "attribute_error", "period projection is read-only");
#endif
#if USE_BERRY_ENERGY_ACCOUNTING_API
    /** @brief Daily properties are owner projections, never writable measurement injection. */
    if (field == "daily" || field == "daily_sum" || field == "yesterday_sum")
        be_raise(vm, "attribute_error", "daily accounting projections are read-only");
#endif
#if USE_BERRY_ENERGY_BALANCED_API
    /** @brief Balanced projections are read-only even for script-writable virtual meters. */
    if (field == "daily_sum_import_balanced" || field == "daily_sum_export_balanced")
        be_raise(vm, "attribute_error", "balanced daily accounting projections are read-only");
#endif
    if (field == "energy_active_export") {
        /** @brief Owner runtime control is allowed on hardware without granting measurement
         * or provider-option writes. It never saves options or changes counter classification.
         */
        const auto enabled = number(vm, 4);
        if (enabled != 0 && enabled != 1)
            be_raise(vm, "value_error", "energy reporting expects zero or one");
        if (!service().set_export_reporting(enabled != 0, current.generation))
            be_raise(vm, "value_error", "energy instance changed or reporting unavailable");
        be_return_nil(vm);
    }
    /** @brief A source ctypes array write is transient, not a persisted export seed. Never
     * reinterpret it as physical counter input while the public field denotes local energy.
     */
    if (field == "export_active" && service().export_reporting())
        be_raise(vm, "attribute_error", "local export projection is read-only; use seed_export");
    /** @brief Source total-array writes are transient. Projection seeds belong to EnergyTotal;
     * physical imported-account writes use the explicit Newmota import_total property.
     */
    if (field == "total" || field == "total_sum" || field == "import_total_sum" ||
        field == "export_total_sum")
        be_raise(vm, "attribute_error", "total projection and account sums are read-only");
    writable(vm, current);
    const auto value = number(vm, 4);
    unsigned mask{};
    if (field == "voltage_common")
        mask = 1;
    else if (field == "frequency_common")
        mask = 2;
    else if (field == "type_dc")
        mask = 4;
    else if (field == "voltage_available")
        mask = 8;
    else if (field == "current_available")
        mask = 16;
    else if (field == "counter_mode")
        mask = 32;
    else if (field == "counter_export_routing")
        mask = 64;
    if (mask) {
        if (value != 0 && value != 1)
            be_raise(vm, "value_error", "energy policy expects zero or one");
        auto options = current.options;
        options.flags = value ? options.flags | mask : options.flags & ~mask;
        if (!service().set_options(options))
            be_raise(vm, "io_error", "energy options save failed");
        be_return_nil(vm);
    }
    if (field == "import_total" || field == "export_total") {
        const auto total = from_kwh(vm, value);
        if (!service().script_totals(channel, current.generation,
                field == "import_total" ? total : current.account.imported,
                field == "export_total" ? total : current.account.exported))
            be_raise(vm, "io_error", "energy total save failed");
        be_return_nil(vm);
    }
    auto sample = current.sample;
    sample.increment_fields = sample.counter_fields = 0;
    sample.pulse_count = 0;
    sample.import_floor_valid = false;
    sample.available_fields = 7;
    const auto scaled = value * 1000;
    // This bound also makes a retained 60-second power increment fit uint64.
    if (scaled < -static_cast<long double>(INT64_MAX / 60000) ||
        scaled > static_cast<long double>(INT64_MAX / 60000))
        be_raise(vm, "value_error", "energy measurement out of range");
    /** @brief Convert the already range-checked scaled value to double and use global llround
     * for nearest, half-away-from-zero rounding; the pinned toolchain omits its std export.
     * The existing bound keeps the rounded result inside long long and the accounting range.
     */
    const auto canonical = static_cast<std::int64_t>(::llround(static_cast<double>(scaled)));
    // These fields are 32-bit; only power-like fields keep the wider accounting range.
    if ((field == "voltage" || field == "current" || field == "power_factor" ||
            field == "temperature" || field == "battery_voltage" || field == "battery_current") &&
        !fits_i32(canonical))
        be_raise(vm, "value_error", "energy measurement out of range");
    if (field == "voltage")
        sample.voltage = static_cast<std::int32_t>(canonical);
    else if (field == "current")
        sample.current = static_cast<std::int32_t>(canonical);
    else if (field == "active_power")
        sample.power = canonical;
    else if (field == "apparent_power") {
        sample.apparent_mva = canonical;
        sample.auxiliary_fields |= 1U << 4;
    } else if (field == "reactive_power") {
        sample.reactive_mvar = canonical;
        sample.auxiliary_fields |= 1U << 5;
    } else if (field == "power_factor") {
        if (value < -1 || value > 1)
            be_raise(vm, "value_error", "power factor out of range");
        sample.power_factor_milli = static_cast<std::int32_t>(canonical);
        sample.auxiliary_fields |= 2;
    } else if (field == "frequency") {
        if (canonical < 0 || canonical > UINT32_MAX)
            be_raise(vm, "value_error", "frequency out of range");
        sample.frequency_millihz = canonical;
        sample.auxiliary_fields |= 1;
    } else if (field == "import_active") {
        sample.import_counter = from_kwh(vm, value);
        sample.counter_fields = 1;
    } else if (field == "export_active" || field == "export_counter") {
        sample.export_counter = from_kwh(vm, value);
        sample.counter_fields = 2;
    } else if (field == "valid")
        sample.valid = value != 0;
    else if (field == "data_valid") {
        if (value < 0 || value > 255 || value != std::floor(value))
            be_raise(vm, "value_error", "invalid validity marker");
        sample.valid = value == 0;
    } else if (field == "temperature") {
        sample.temperature_millicelsius = static_cast<std::int32_t>(canonical);
        sample.auxiliary_fields |= 1U << 16;
    } else if (field == "battery_voltage") {
        sample.battery_voltage_mv = static_cast<std::int32_t>(canonical);
        sample.auxiliary_fields |= 1U << 17;
    } else if (field == "battery_current") {
        sample.battery_current_ma = static_cast<std::int32_t>(canonical);
        sample.auxiliary_fields |= 1U << 18;
    } else
        be_raise(vm, "attribute_error",
            "energy field is read-only or has no implemented service mapping");
    if (!service().script_inject(channel, current.generation, sample))
        be_raise(vm, "value_error", "energy publication rejected");
    be_return_nil(vm);
}

int Enabled(bvm* vm) {
    Snapshot value{};
    be_pushbool(vm, service().snapshot(1, value) && value.driver == 27 && value.writable);
    be_return(vm);
}

int Update(bvm* vm) {
    const auto token = be_top(vm) ? integer(vm, 1, 1, INT32_MAX) : 0;
    (void)snapshot(vm, 1, token);
    be_return_nil(vm);
}

int Exact(bvm* vm) {
    if (be_top(vm) != 2 && be_top(vm) != 7)
        be_raise(vm, "type_error", "expected exact totals query or counter/seed arguments");
    const auto channel = integer(vm, 1, 1, 8);
    const auto current = snapshot(vm, channel, integer(vm, 2, 1, INT32_MAX));
    if (be_top(vm) == 2) {
        be_newobject(vm, "map");
        put_wh(vm, "import_wh", current.account.imported.wh);
        put_integer(vm, "import_micro_wh", current.account.imported.micro_wh);
        put_wh(vm, "export_wh", current.account.exported.wh);
        put_integer(vm, "export_micro_wh", current.account.exported.micro_wh);
        // Remove only the .p backing map; the map instance remains for be_return.
        be_pop(vm, 1);
        be_return(vm);
    }
    writable(vm, current);
    const auto imported = exact(vm, 3, 4), exported = exact(vm, 5, 6);
    if (be_top(vm) == 7 && be_tobool(vm, 7)) {
        if (!service().script_totals(channel, current.generation, imported, exported))
            be_raise(vm, "io_error", "energy seed save failed");
    } else {
        auto sample = current.sample;
        sample.increment_fields = 0;
        sample.pulse_count = 0;
        sample.import_floor_valid = false;
        sample.import_counter = imported;
        sample.export_counter = exported;
        sample.counter_fields = 3;
        sample.available_fields = 7;
        if (!service().script_inject(channel, current.generation, sample))
            be_raise(vm, "value_error", "energy counter publication rejected");
    }
    be_return_nil(vm);
}

#if USE_ENERGY_EXPORT_COMPAT
/** @brief Seeds one checked meter's local export projection through the shared command owner.
 * @param vm Slot, channel, identity generation, signed integer Wh, optional uint32 epoch or nil.
 * @return Berry nil; invalid input, disabled reporting or failed durable save raises an error.
 */
int SeedExport(bvm* vm) {
    const auto channel = integer(vm, 1, 1, Service::kMaximumChannels);
    const auto generation = integer(vm, 2, 1, INT32_MAX);
    (void)snapshot(vm, channel, generation);
    const auto wh = integer(vm, 3, INT32_MIN, INT32_MAX);
    char payload[40];
    if (be_top(vm) >= 4 && !be_isnil(vm, 4)) {
        const auto timestamp = number(vm, 4);
        if (timestamp < 0 || timestamp > UINT32_MAX || timestamp != std::floor(timestamp))
            be_raise(vm, "value_error", "export seed timestamp out of range");
        std::snprintf(payload, sizeof(payload), "%ld,%lu", static_cast<long>(wh),
            static_cast<unsigned long>(timestamp));
    } else {
        std::snprintf(payload, sizeof(payload), "%ld", static_cast<long>(wh));
    }
    if (!service().seed_export(channel, payload, generation))
        be_raise(vm, "io_error", "export seed rejected or save failed; reporting must be enabled");
    be_return_nil(vm);
}
#endif

int PhaseKey(bvm* vm) {
    const auto key = text(vm, 1);
    constexpr std::string_view suffix = "_phases";
    if (key.size() > suffix.size() && key.substr(key.size() - suffix.size()) == suffix) {
        /** @brief Meter policy metadata has no channel view, even on virtual providers. */
        const auto field = key.substr(0, key.size() - suffix.size());
        if (field == "voltage_common" || field == "frequency_common" || field == "type_dc" ||
            field == "voltage_available" || field == "current_available")
            be_raise(vm, "attribute_error", "meter metadata is scalar");
        be_pushnstring(vm, key.data(), key.size() - suffix.size());
        be_return(vm);
    }
    be_return_nil(vm);
}

/** @brief Safe proxy glue; only scalar values and generation tokens cross native calls. */
constexpr char proxies[] =
    R"be(
class EnergyPhaseView
  var generation, field, count
  def init(g,f,n) self.generation=g self.field=f self.count=n end
  def size() return _energy_native._read('phase_count',1,self.generation) end
  def item(i)
    if i < 0 i += self.count end
    if i < 0 || i >= self.count raise 'index_error','energy phase out of range' end
    return _energy_native._read(self.field,i+1,self.generation)
  end
  def setitem(i,v)
    if i < 0 i += self.count end
    if i < 0 || i >= self.count raise 'index_error','energy phase out of range' end
    _energy_native._write(self.field,i+1,self.generation,v)
  end
  def tostring()
    var a=[]
    for i:0..self.count-1 a.push(self.item(i)) end
    return str(a)
  end
end
class EnergyProvider
  var generation, count
  def init()
    var info=_energy_native._info()
    if info == nil raise 'value_error','no energy provider is selected' end
    self.generation=info['generation'] self.count=info['phase_count']
  end
  def member(k)
    var f=_energy_native._phase_key(k)
    if f != nil return EnergyPhaseView(self.generation,f,self.count) end
    return _energy_native._read(k,1,self.generation)
  end
  def setmember(k,v)
    _energy_native._write(k,1,self.generation,v)
  end
  def read()
    var m={}
    var fields=['voltage','current','active_power','apparent_power','reactive_power','power_factor','frequency','import_active','export_active','export_counter','total','import_total','export_total','start_energy','data_valid']
)be"
#if USE_BERRY_ENERGY_PERIOD_API
    R"be(    fields.push('period')
)be"
#endif
#if USE_BERRY_ENERGY_ACCOUNTING_API
    R"be(    fields.push('daily')
)be"
#endif
    R"be(    for f:fields
      m[f]=_energy_native._read(f,1,self.generation)
      var a=[]
      for i:1..self.count a.push(_energy_native._read(f,i,self.generation)) end
      m[f+'_phases']=a
      if self.count > 1
        for i:2..self.count m[f+'_'+str(i)]=_energy_native._read(f,i,self.generation) end
      end
    end
)be"
#if USE_BERRY_ENERGY_ACCOUNTING_API
    R"be(    m['daily_sum']=_energy_native._read('daily_sum',1,self.generation)
    m['yesterday_sum']=_energy_native._read('yesterday_sum',1,self.generation)
)be"
#endif
#if USE_BERRY_ENERGY_BALANCED_API
    R"be(    m['daily_sum_import_balanced']=_energy_native._read('daily_sum_import_balanced',1,self.generation)
    m['daily_sum_export_balanced']=_energy_native._read('daily_sum_export_balanced',1,self.generation)
)be"
#endif
    R"be(    m['total_sum']=_energy_native._read('total_sum',1,self.generation)
    m['import_total_sum']=_energy_native._read('import_total_sum',1,self.generation)
    m['export_total_sum']=_energy_native._read('export_total_sum',1,self.generation)
    m['type_dc']=_energy_native._read('type_dc',1,self.generation)
    m['voltage_available']=_energy_native._read('voltage_available',1,self.generation)
    m['current_available']=_energy_native._read('current_available',1,self.generation)
    m['voltage_common']=_energy_native._read('voltage_common',1,self.generation)
    m['frequency_common']=_energy_native._read('frequency_common',1,self.generation)
    m['power_on']=_energy_native._read('power_on',1,self.generation)
    m['use_overtemp']=_energy_native._read('use_overtemp',1,self.generation)
    m['energy_active_export']=_energy_native._read('energy_active_export',1,self.generation)
    m['phase_count']=self.count
    m['driver_id']=_energy_native._read('driver_id',1,self.generation)
    return m
  end
  def tomap() return self.read() end
  def update_total() _energy_native._update(self.generation) end
  def driver_enabled() _energy_native._update(self.generation) return _energy_native._enabled() end
  def mode(v)
    if v != 'power' && v != 'counter' raise 'value_error','mode must be power or counter' end
    self.setmember('counter_mode',v == 'counter')
  end
  def totals(c) return _energy_native._exact(c,self.generation) end
  def counter(c,w,u,x,y) _energy_native._exact(c,self.generation,w,u,x,y,false) end
  def seed(c,w,u,x,y) _energy_native._exact(c,self.generation,w,u,x,y,true) end
)be"
#if USE_ENERGY_EXPORT_COMPAT
    R"be(  def seed_export(c,w,t) _energy_native._seed_export(c,self.generation,w,t) end
)be"
#endif
    R"be(end
class EnergyModule
  def init(m) return m end
  def member(k)
    if k == '_phases' return 8 end
    return EnergyProvider().member(k)
  end
  def setmember(k,v) EnergyProvider().setmember(k,v) end
  def read()
    if _energy_native._info() == nil return nil end
    return EnergyProvider().read()
  end
  def tomap() return self.read() end
  def driver_enabled() return _energy_native._enabled() end
  def update_total() _energy_native._update() end
  def mode(v) EnergyProvider().mode(v) end
  def totals(c) return EnergyProvider().totals(c) end
  def counter(c,w,u,x,y) EnergyProvider().counter(c,w,u,x,y) end
  def seed(c,w,u,x,y) EnergyProvider().seed(c,w,u,x,y) end
)be"
#if USE_ENERGY_EXPORT_COMPAT
    R"be(  def seed_export(c,w,t) EnergyProvider().seed_export(c,w,t) end
)be"
#endif
    R"be(  def _deref() return nil end
end
energy=EnergyModule()
)be";
} // namespace

namespace newmota {
bool install_energy_module(bvm* vm) {
    be_newmodule(vm);
    be_setname(vm, -1, "_energy_native");
    const bnfuncinfo functions[] = {{"_info", Info}, {"_read", Read}, {"_write", Write},
        {"_enabled", Enabled}, {"_update", Update}, {"_exact", Exact}, {"_phase_key", PhaseKey},
#if USE_ENERGY_EXPORT_COMPAT
        {"_seed_export", SeedExport},
#endif
        {nullptr, nullptr}};
    for (const auto* f = functions; f->name; ++f) {
        be_pushntvfunction(vm, f->function);
        if (!be_setmember(vm, -2, f->name)) {
            be_pop(vm, 2);
            return false;
        }
        be_pop(vm, 1);
    }
    be_setglobal(vm, "_energy_native");
    be_setglobal(vm, "energy");
    be_pop(vm, 1);
    const auto top = be_top(vm);
    const auto result = be_loadbuffer(vm, "energy_proxies", proxies, sizeof(proxies) - 1);
    const bool success = result == BE_OK && be_pcall(vm, 0) == BE_OK;
    be_pop(vm, be_top(vm) - top);
    if (success) {
        be_getglobal(vm, "energy");
        be_cache_module(vm, be_newstr(vm, "energy"));
        be_pop(vm, 1);
    }
    return success;
}
} // namespace newmota
